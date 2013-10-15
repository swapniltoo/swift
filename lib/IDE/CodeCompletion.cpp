#include "swift/IDE/CodeCompletion.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ModuleLoadListener.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Optional.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Sema/CodeCompletionTypeChecking.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/SaveAndRestore.h"
#include "clang/Basic/Module.h"
#include <algorithm>
#include <functional>
#include <string>

using namespace swift;
using namespace ide;

std::string swift::ide::removeCodeCompletionTokens(
    StringRef Input, StringRef TokenName, unsigned *CompletionOffset) {
  assert(TokenName.size() >= 1);

  *CompletionOffset = ~0U;

  std::string CleanFile;
  CleanFile.reserve(Input.size());
  const std::string Token = std::string("#^") + TokenName.str() + "^#";

  for (const char *Ptr = Input.begin(), *End = Input.end();
       Ptr != End; ++Ptr) {
    const char C = *Ptr;
    if (C == '#' && Ptr <= End - Token.size() &&
        StringRef(Ptr, Token.size()) == Token) {
      Ptr += Token.size() - 1;
      *CompletionOffset = CleanFile.size();
      CleanFile += '\0';
      continue;
    }
    if (C == '#' && Ptr <= End - 2 && Ptr[1] == '^') {
      do {
        Ptr++;
      } while(*Ptr != '#');
      continue;
    }
    CleanFile += C;
  }
  return CleanFile;
}

CodeCompletionString::CodeCompletionString(ArrayRef<Chunk> Chunks) {
  Chunk *TailChunks = reinterpret_cast<Chunk *>(this + 1);
  std::copy(Chunks.begin(), Chunks.end(), TailChunks);
  NumChunks = Chunks.size();
}

void CodeCompletionString::print(raw_ostream &OS) const {
  unsigned PrevNestingLevel = 0;
  for (auto C : getChunks()) {
    if (C.getNestingLevel() < PrevNestingLevel) {
      OS << "#}";
    }
    switch (C.getKind()) {
    case Chunk::ChunkKind::Text:
    case Chunk::ChunkKind::LeftParen:
    case Chunk::ChunkKind::RightParen:
    case Chunk::ChunkKind::LeftBracket:
    case Chunk::ChunkKind::RightBracket:
    case Chunk::ChunkKind::LeftAngle:
    case Chunk::ChunkKind::RightAngle:
    case Chunk::ChunkKind::Dot:
    case Chunk::ChunkKind::Comma:
    case Chunk::ChunkKind::ExclamationMark:
    case Chunk::ChunkKind::QuestionMark:
    case Chunk::ChunkKind::CallParameterName:
    case Chunk::ChunkKind::CallParameterColon:
    case Chunk::ChunkKind::CallParameterType:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterName:
      OS << C.getText();
      break;
    case Chunk::ChunkKind::OptionalBegin:
    case Chunk::ChunkKind::CallParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin:
      OS << "{#";
      break;
    case Chunk::ChunkKind::DynamicLookupMethodCallTail:
      OS << "{#" << C.getText() << "#}";
      break;
    case Chunk::ChunkKind::TypeAnnotation:
      OS << "[#";
      OS << C.getText();
      OS << "#]";
      break;
    }
    PrevNestingLevel = C.getNestingLevel();
  }
  while (PrevNestingLevel > 0) {
    OS << "#}";
    PrevNestingLevel--;
  }
}

void CodeCompletionString::dump() const {
  print(llvm::errs());
}

void CodeCompletionResult::print(raw_ostream &OS) const {
  switch (getKind()) {
  case ResultKind::Declaration:
    OS << "Decl/";
    break;
  case ResultKind::Keyword:
    OS << "Keyword/";
    break;
  case ResultKind::Pattern:
    OS << "Pattern/";
    break;
  }
  switch (getSemanticContext()) {
  case SemanticContextKind::None:
    OS << "None:         ";
    break;
  case SemanticContextKind::ExpressionSpecific:
    OS << "ExprSpecific: ";
    break;
  case SemanticContextKind::Local:
    OS << "Local:        ";
    break;
  case SemanticContextKind::CurrentNominal:
    OS << "CurrNominal:  ";
    break;
  case SemanticContextKind::Super:
    OS << "Super:        ";
    break;
  case SemanticContextKind::OutsideNominal:
    OS << "OutNominal:   ";
    break;
  case SemanticContextKind::CurrentModule:
    OS << "CurrModule:   ";
    break;
  case SemanticContextKind::OtherModule:
    OS << "OtherModule:  ";
    break;
  }
  CompletionString->print(OS);
}

void CodeCompletionResult::dump() const {
  print(llvm::errs());
}

void CodeCompletionResultBuilder::addChunkWithText(
    CodeCompletionString::Chunk::ChunkKind Kind, StringRef Text) {
  Chunks.push_back(CodeCompletionString::Chunk::createWithText(
      Kind, CurrentNestingLevel, Context.copyString(Text)));
}

CodeCompletionResult *CodeCompletionResultBuilder::takeResult() {
  void *CCSMem = Context.Allocator
      .Allocate(sizeof(CodeCompletionString) +
                    Chunks.size() * sizeof(CodeCompletionString::Chunk),
                llvm::alignOf<CodeCompletionString>());
  auto *CCS = new (CCSMem) CodeCompletionString(Chunks);

  switch (Kind) {
  case CodeCompletionResult::ResultKind::Declaration:
    return new (Context.Allocator) CodeCompletionResult(SemanticContext, CCS,
                                                        AssociatedDecl);

  case CodeCompletionResult::ResultKind::Keyword:
  case CodeCompletionResult::ResultKind::Pattern:
    return new (Context.Allocator) CodeCompletionResult(Kind, SemanticContext,
                                                        CCS);
  }
}

void CodeCompletionResultBuilder::finishResult() {
  Context.addResult(takeResult(), HasLeadingDot);
}

namespace swift {
namespace ide {
struct CodeCompletionContext::ClangCacheImpl {
  CodeCompletionContext &CompletionContext;

  /// Per-module completion result cache for results with a leading dot.
  llvm::DenseMap<const ClangModule *, std::vector<CodeCompletionResult *>>
      CacheWithDot;

  /// Per-module completion result cache for results without a leading dot.
  llvm::DenseMap<const ClangModule *, std::vector<CodeCompletionResult *>>
      CacheWithoutDot;

  /// A function that can generate results for the cache.
  std::function<void(bool NeedLeadingDot)> RefillCache;

  struct RequestedResultsTy {
    const ClangModule *Module;
    bool NeedLeadingDot;
  };

  /// Describes the results requested for current completion.
  Optional<RequestedResultsTy> RequestedResults = Nothing;

  ClangCacheImpl(CodeCompletionContext &CompletionContext)
      : CompletionContext(CompletionContext) {}

  void clear() {
    CacheWithDot.clear();
    CacheWithoutDot.clear();
  }

  void ensureCached() {
  if (!RequestedResults)
    return;

    bool NeedDot = RequestedResults->NeedLeadingDot;
    if ((NeedDot && CacheWithDot.empty()) ||
        (!NeedDot && CacheWithoutDot.empty())) {
      llvm::SaveAndRestore<ResultDestination> ChangeDestination(
          CompletionContext.CurrentDestination, ResultDestination::ClangCache);

      RefillCache(NeedDot);
    }
  }

  void requestUnqualifiedResults() {
    RequestedResults = RequestedResultsTy{nullptr, false};
  }

  void requestQualifiedResults(const ClangModule *Module,
                               bool NeedLeadingDot) {
    assert(Module && "should specify a non-null Module");
    RequestedResults = RequestedResultsTy{Module, NeedLeadingDot};
  }

  void addResult(CodeCompletionResult *R, bool HasLeadingDot);
  void takeResults();
};
} // namespace ide
} // namespace swift

void CodeCompletionContext::ClangCacheImpl::addResult(CodeCompletionResult *R,
                                                      bool HasLeadingDot) {
  const ClangModule *Module = nullptr;
  if (auto *D = R->getAssociatedDecl())
    Module = cast<ClangModule>(D->getModuleContext());
  auto &Cache = HasLeadingDot ? CacheWithDot : CacheWithoutDot;
  Cache[Module].push_back(R);
}

void CodeCompletionContext::ClangCacheImpl::takeResults() {
  if (!RequestedResults)
    return;

  ensureCached();

  std::vector<CodeCompletionResult *> &Results =
      CompletionContext.CurrentCompletionResults;
  // Add Clang results from the cache.
  auto *Module = RequestedResults->Module;
  auto &Cache = RequestedResults->NeedLeadingDot ? CacheWithDot :
                                                   CacheWithoutDot;
  for (auto KV : Cache) {
    // Include results from this module if:
    // * no particular module was requested, or
    // * this module was specifically requested, or
    // * this module is re-exported from the requested module.
    if (!Module || Module == KV.first ||
        Module->getClangModule()->isModuleVisible(KV.first->getClangModule()))
      for (auto R : KV.second)
        Results.push_back(R);
  }

  RequestedResults = Nothing;
}

CodeCompletionContext::CodeCompletionContext()
    : ClangResultCache(new ClangCacheImpl(*this)) {}

CodeCompletionContext::~CodeCompletionContext() {}

StringRef CodeCompletionContext::copyString(StringRef String) {
  char *Mem = Allocator.Allocate<char>(String.size());
  std::copy(String.begin(), String.end(), Mem);
  return StringRef(Mem, String.size());
}

MutableArrayRef<CodeCompletionResult *> CodeCompletionContext::takeResults() {
  ClangResultCache->takeResults();

  // Copy pointers to the results.
  const size_t Count = CurrentCompletionResults.size();
  CodeCompletionResult **Results =
      Allocator.Allocate<CodeCompletionResult *>(Count);
  std::copy(CurrentCompletionResults.begin(), CurrentCompletionResults.end(),
            Results);
  CurrentCompletionResults.clear();
  return MutableArrayRef<CodeCompletionResult *>(Results, Count);
}

namespace {
StringRef getFirstTextChunk(CodeCompletionResult *R) {
  for (auto C : R->getCompletionString()->getChunks()) {
    switch (C.getKind()) {
    case CodeCompletionString::Chunk::ChunkKind::Text:
    case CodeCompletionString::Chunk::ChunkKind::LeftParen:
    case CodeCompletionString::Chunk::ChunkKind::RightParen:
    case CodeCompletionString::Chunk::ChunkKind::LeftBracket:
    case CodeCompletionString::Chunk::ChunkKind::RightBracket:
    case CodeCompletionString::Chunk::ChunkKind::LeftAngle:
    case CodeCompletionString::Chunk::ChunkKind::RightAngle:
    case CodeCompletionString::Chunk::ChunkKind::Dot:
    case CodeCompletionString::Chunk::ChunkKind::Comma:
    case CodeCompletionString::Chunk::ChunkKind::ExclamationMark:
    case CodeCompletionString::Chunk::ChunkKind::QuestionMark:
      return C.getText();

    case CodeCompletionString::Chunk::ChunkKind::CallParameterName:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterColon:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterType:
    case CodeCompletionString::Chunk::ChunkKind::OptionalBegin:
    case CodeCompletionString::Chunk::ChunkKind::CallParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterBegin:
    case CodeCompletionString::Chunk::ChunkKind::GenericParameterName:
    case CodeCompletionString::Chunk::ChunkKind::DynamicLookupMethodCallTail:
    case CodeCompletionString::Chunk::ChunkKind::TypeAnnotation:
      continue;
    }
  }
  return StringRef();
}
} // unnamed namespace

void CodeCompletionContext::addResult(CodeCompletionResult *R,
                                      bool HasLeadingDot) {
  switch (CurrentDestination) {
  case ResultDestination::CurrentSet:
    CurrentCompletionResults.push_back(R);
    return;

  case ResultDestination::ClangCache:
    ClangResultCache->addResult(R, HasLeadingDot);
    return;
  }
}

void CodeCompletionContext::sortCompletionResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  std::sort(Results.begin(), Results.end(),
            [](CodeCompletionResult *LHS, CodeCompletionResult *RHS) {
    StringRef LHSChunk = getFirstTextChunk(LHS);
    StringRef RHSChunk = getFirstTextChunk(RHS);
    int Result = LHSChunk.compare_lower(RHSChunk);
    // If the case insensitive comparison is equal, then secondary sort order
    // should be case sensitive.
    if (Result == 0)
      Result = LHSChunk.compare(RHSChunk);
    return Result < 0;
  });
}

void CodeCompletionContext::clearClangCache() {
  ClangResultCache->clear();
}

void CodeCompletionContext::setCacheClangResults(
    std::function<void(bool NeedLeadingDot)> RefillCache) {
  ClangResultCache->RefillCache = RefillCache;
}

void CodeCompletionContext::includeUnqualifiedClangResults() {
  ClangResultCache->requestUnqualifiedResults();
}

void
CodeCompletionContext::includeQualifiedClangResults(const ClangModule *Module,
                                                    bool NeedLeadingDot) {
  ClangResultCache->requestQualifiedResults(Module, NeedLeadingDot);
}

namespace {
class CodeCompletionCallbacksImpl : public CodeCompletionCallbacks,
                                    public ModuleLoadListener {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

  enum class CompletionKind {
    None,
    DotExpr,
    PostfixExprBeginning,
    PostfixExpr,
    SuperExpr,
    SuperExprDot,
    TypeSimpleBeginning,
    TypeIdentifierWithDot,
    TypeIdentifierWithoutDot,
  };

  CompletionKind Kind = CompletionKind::None;
  Expr *ParsedExpr = nullptr;
  TypeLoc ParsedTypeLoc;
  DeclContext *CurDeclContext = nullptr;
  Decl *CStyleForLoopIterationVariable = nullptr;

  /// \brief Set to true when we have delivered code completion results
  /// to the \c Consumer.
  bool DeliveredResults = false;

  bool typecheckContextImpl(DeclContext *DC) {
    // Type check the function that contains the expression.
    if (DC->getContextKind() == DeclContextKind::AbstractClosureExpr ||
        DC->getContextKind() == DeclContextKind::AbstractFunctionDecl) {
      SourceLoc EndTypeCheckLoc =
          ParsedExpr ? ParsedExpr->getStartLoc()
                     : P.Context.SourceMgr.getCodeCompletionLoc();
      // Find the nearest outer function.
      DeclContext *DCToTypeCheck = DC;
      while (!DCToTypeCheck->isModuleContext() &&
             !isa<AbstractFunctionDecl>(DCToTypeCheck))
        DCToTypeCheck = DCToTypeCheck->getParent();
      // First, type check the nominal decl that contains the function.
      typecheckContextImpl(DCToTypeCheck->getParent());
      // Then type check the function itself.
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(DCToTypeCheck))
        return typeCheckAbstractFunctionBodyUntil(AFD, EndTypeCheckLoc);
      return false;
    }
    if (DC->getContextKind() == DeclContextKind::NominalTypeDecl) {
      auto *NTD = cast<NominalTypeDecl>(DC);
      // First, type check the parent DeclContext.
      typecheckContextImpl(DC->getParent());
      if (NTD->hasType())
        return true;
      return typeCheckCompletionDecl(cast<NominalTypeDecl>(DC));
    }
    if (DC->getContextKind() == DeclContextKind::TopLevelCodeDecl) {
      return typeCheckTopLevelCodeDecl(dyn_cast<TopLevelCodeDecl>(DC));
    }
    return true;
  }

  /// \returns true on success, false on failure.
  bool typecheckContext() {
    return typecheckContextImpl(CurDeclContext);
  }

  /// \returns true on success, false on failure.
  bool typecheckDelayedParsedDecl() {
    assert(DelayedParsedDecl && "should have a delayed parsed decl");
    return typeCheckCompletionDecl(DelayedParsedDecl);
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedExpr() {
    assert(ParsedExpr && "should have an expression");

    DEBUG(llvm::dbgs() << "\nparsed:\n";
          ParsedExpr->print(llvm::dbgs());
          llvm::dbgs() << "\n");

    Expr *TypecheckedExpr = ParsedExpr;
    if (!typeCheckCompletionContextExpr(P.Context, CurDeclContext,
                                        TypecheckedExpr))
      return false;

    if (TypecheckedExpr->getType()->is<ErrorType>())
      return false;

    ParsedExpr = TypecheckedExpr;

    DEBUG(llvm::dbgs() << "\type checked:\n";
          ParsedExpr->print(llvm::dbgs());
          llvm::dbgs() << "\n");
    return true;
  }

  /// \returns true on success, false on failure.
  bool typecheckParsedType() {
    assert(ParsedTypeLoc.getTypeRepr() && "should have a TypeRepr");
    return !performTypeLocChecking(P.Context, ParsedTypeLoc, CurDeclContext,
                                   false);
  }

public:
  CodeCompletionCallbacksImpl(Parser &P,
                              CodeCompletionContext &CompletionContext,
                              CodeCompletionConsumer &Consumer)
      : CodeCompletionCallbacks(P), CompletionContext(CompletionContext),
        Consumer(Consumer) {
    P.Context.addModuleLoadListener(*this);
  }

  ~CodeCompletionCallbacksImpl() {
    P.Context.removeModuleLoadListener(*this);
  }

  void completeExpr() override;
  void completeDotExpr(Expr *E) override;
  void completePostfixExprBeginning() override;
  void completePostfixExpr(Expr *E) override;
  void completeExprSuper(SuperRefExpr *SRE) override;
  void completeExprSuperDot(SuperRefExpr *SRE) override;

  void completeTypeSimpleBeginning() override;
  void completeTypeIdentifierWithDot(IdentTypeRepr *ITR) override;
  void completeTypeIdentifierWithoutDot(IdentTypeRepr *ITR) override;

  void doneParsing() override;

  void deliverCompletionResults();

  // Implement swift::ModuleLoadListener.
  void loadedModule(ModuleLoader *Loader, Module *M) override;
};
} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeExpr() {
  if (DeliveredResults)
    return;

  Parser::ParserPositionRAII RestorePosition(P);
  P.restoreParserPosition(ExprBeginPosition);

  // FIXME: implement fallback code completion.

  deliverCompletionResults();
}

namespace {
/// Build completions by doing visible decl lookup from a context.
class CompletionLookup : swift::VisibleDeclConsumer {
  CodeCompletionContext &CompletionContext;
  ASTContext &Ctx;
  OwnedResolver TypeResolver;
  Identifier SelfIdent;
  const DeclContext *CurrDeclContext;

  enum class LookupKind {
    ValueExpr,
    ValueInDeclContext,
    Type,
    TypeInDeclContext,
  };

  LookupKind Kind;

  /// Type of the user-provided expression for LookupKind::ValueExpr
  /// completions.
  Type ExprType;

  /// User-provided base type for LookupKind::Type completions.
  Type BaseType;

  bool HaveDot = false;
  bool NeedLeadingDot = false;
  bool IsSuperRefExpr = false;
  bool IsDynamicLookup = false;

  /// \brief True if we are code completing inside a static method.
  bool InsideStaticMethod = false;

  /// \brief Innermost method that the code completion point is in.
  const AbstractFunctionDecl *CurrentMethod = nullptr;

  /// \brief Declarations that should get ExpressionSpecific semantic context.
  llvm::SmallSet<const Decl *, 4> ExpressionSpecificDecls;

  bool needDot() const {
    return NeedLeadingDot;
  }

public:
  CompletionLookup(CodeCompletionContext &CompletionContext,
                   ASTContext &Ctx,
                   const DeclContext *CurrDeclContext)
      : CompletionContext(CompletionContext), Ctx(Ctx),
        TypeResolver(createLazyResolver(Ctx)),
        SelfIdent(Ctx.getIdentifier("self")),
        CurrDeclContext(CurrDeclContext) {
    // Determine if we are doing code completion inside a static method.
    if (CurrDeclContext->isLocalContext()) {
      const DeclContext *FunctionDC = CurrDeclContext;
      while (FunctionDC->isLocalContext()) {
        const DeclContext *Parent = FunctionDC->getParent();
        if (!Parent->isLocalContext())
          break;
        FunctionDC = Parent;
      }
      if (auto *AFD = dyn_cast<AbstractFunctionDecl>(FunctionDC)) {
        if (AFD->getExtensionType()) {
          CurrentMethod = AFD;
          if (auto *FD = dyn_cast<FuncDecl>(AFD))
            InsideStaticMethod = FD->isStatic();
        }
      }
    }

    CompletionContext.setCacheClangResults([&](bool NeedLeadingDot) {
      llvm::SaveAndRestore<bool> S(this->NeedLeadingDot, NeedLeadingDot);
      if (auto Importer = Ctx.getClangModuleLoader())
        static_cast<ClangImporter &>(*Importer).lookupVisibleDecls(*this);
    });
  }

  void setHaveDot() {
    HaveDot = true;
  }

  void setIsSuperRefExpr() {
    IsSuperRefExpr = true;
  }

  void setIsDynamicLookup() {
    IsDynamicLookup = true;
  }

  void addExpressionSpecificDecl(const Decl *D) {
    ExpressionSpecificDecls.insert(D);
  }

  SemanticContextKind getSemanticContext(const Decl *D,
                                         DeclVisibilityKind Reason) {
    switch (Reason) {
    case DeclVisibilityKind::LocalVariable:
    case DeclVisibilityKind::FunctionParameter:
    case DeclVisibilityKind::GenericParameter:
      if (ExpressionSpecificDecls.count(D))
        return SemanticContextKind::ExpressionSpecific;
      return SemanticContextKind::Local;

    case DeclVisibilityKind::MemberOfCurrentNominal:
      if (IsSuperRefExpr &&
          CurrentMethod && CurrentMethod->getOverriddenDecl() == D)
        return SemanticContextKind::ExpressionSpecific;
      return SemanticContextKind::CurrentNominal;

    case DeclVisibilityKind::MemberOfSuper:
      return SemanticContextKind::Super;

    case DeclVisibilityKind::MemberOfOutsideNominal:
      return SemanticContextKind::OutsideNominal;

    case DeclVisibilityKind::VisibleAtTopLevel:
      if (D->getModuleContext() == CurrDeclContext->getParentModule())
        return SemanticContextKind::CurrentModule;
      else
        return SemanticContextKind::OtherModule;

    case DeclVisibilityKind::DynamicLookup:
      // DynamicLookup results can come from different modules, including the
      // current module, but we always assign them the OtherModule semantic
      // context.  These declarations are uniqued by signature, so it is
      // totally random (determined by the hash function) which of the
      // equivalent declarations (across multiple modules) we will get.
      return SemanticContextKind::OtherModule;
    }
    llvm_unreachable("unhandled kind");
  }

  void addTypeAnnotation(CodeCompletionResultBuilder &Builder, Type T) {
    if (T->isVoid())
      Builder.addTypeAnnotation("Void");
    else
      Builder.addTypeAnnotation(T.getString());
  }

  void addVarDeclRef(const VarDecl *VD, DeclVisibilityKind Reason) {
    StringRef Name = VD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    assert(!(InsideStaticMethod &&
            VD->getDeclContext() == CurrentMethod->getDeclContext()) &&
           "name lookup bug -- can not see an instance variable "
           "in a static function");
    // FIXME: static variables.

    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(VD, Reason));
    Builder.setAssociatedDecl(VD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);

    // Add a type annotation.
    Type T = VD->getType();
    if (VD->getName() == SelfIdent) {
      // Strip [inout] from 'self'.  It is useful to show [inout] for function
      // parameters.  But for 'self' it is just noise.
      T = T->getRValueType();
    }
    if (IsDynamicLookup) {
      // Values of properties that were found on a DynamicLookup have
      // Optional<T> type.
      T = OptionalType::get(T, Ctx);
    }
    addTypeAnnotation(Builder, T);
  }

  void addPatternParameters(CodeCompletionResultBuilder &Builder,
                            const Pattern *P) {
    if (auto *TP = dyn_cast<TuplePattern>(P)) {
      bool NeedComma = false;
      for (auto TupleElt : TP->getFields()) {
        if (NeedComma)
          Builder.addComma(", ");
        StringRef BoundNameStr;
        Builder.addCallParameter(TupleElt.getPattern()->getBoundName(),
                                 TupleElt.getPattern()->getType().getString());
        NeedComma = true;
      }
      return;
    }
    if (auto *PP = dyn_cast<ParenPattern>(P)) {
      Builder.addCallParameter(PP->getBoundName(), PP->getType().getString());
      return;
    }
    auto *TP = cast<TypedPattern>(P);
    Builder.addCallParameter(TP->getBoundName(), TP->getType().getString());
  }

  void addFunctionCall(const AnyFunctionType *AFT) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Pattern,
        SemanticContextKind::ExpressionSpecific);
    Builder.addLeftParen();
    bool NeedComma = false;
    if (auto *TT = AFT->getInput()->getAs<TupleType>()) {
      for (auto TupleElt : TT->getFields()) {
        if (NeedComma)
          Builder.addComma(", ");
        Builder.addCallParameter(TupleElt.getName(),
                                 TupleElt.getType().getString());
        NeedComma = true;
      }
    } else {
      Type T = AFT->getInput();
      if (auto *PT = dyn_cast<ParenType>(T.getPointer())) {
        // Only unwrap the paren shugar, if it exists.
        T = PT->getUnderlyingType();
      }
      Builder.addCallParameter(Identifier(), T->getString());
    }
    Builder.addRightParen();
    addTypeAnnotation(Builder, AFT->getResult());
  }

  void addMethodCall(const FuncDecl *FD, DeclVisibilityKind Reason) {
    bool IsImlicitlyCurriedInstanceMethod;
    switch (Kind) {
    case LookupKind::ValueExpr:
      IsImlicitlyCurriedInstanceMethod = ExprType->is<MetaTypeType>() &&
                                         !FD->isStatic();
      break;
    case LookupKind::ValueInDeclContext:
      IsImlicitlyCurriedInstanceMethod =
          CurrentMethod &&
          FD->getDeclContext() == CurrentMethod->getDeclContext() &&
          InsideStaticMethod && !FD->isStatic();
      break;
    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      llvm_unreachable("can not have a method call while doing a "
                       "type completion");
    }

    StringRef Name = FD->getName().get();
    assert(!Name.empty() && "name should not be empty");

    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(FD, Reason));
    Builder.setAssociatedDecl(FD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    if (IsDynamicLookup)
      Builder.addDynamicLookupMethodCallTail();
    Builder.addLeftParen();
    auto Patterns = FD->getArgParamPatterns();
    unsigned FirstIndex = 0;
    if (!IsImlicitlyCurriedInstanceMethod && FD->getImplicitSelfDecl())
      FirstIndex = 1;
    addPatternParameters(Builder, Patterns[FirstIndex]);
    Builder.addRightParen();

    // Build type annotation.
    llvm::SmallString<32> TypeStr;
    {
      llvm::raw_svector_ostream OS(TypeStr);
      for (unsigned i = FirstIndex + 1, e = Patterns.size(); i != e; ++i) {
        Patterns[i]->print(OS);
        OS << " -> ";
      }
      Type ResultType = FD->getResultType(Ctx);
      if (ResultType->isVoid())
        OS << "Void";
      else
        ResultType.print(OS);
    }
    Builder.addTypeAnnotation(TypeStr);

    // TODO: skip arguments with default parameters?
  }

  void addConstructorCall(const ConstructorDecl *CD,
                          DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(CD, Reason));
    Builder.setAssociatedDecl(CD);
     if (IsSuperRefExpr) {
      assert(isa<ConstructorDecl>(
                 dyn_cast<AbstractFunctionDecl>(CurrDeclContext)) &&
             "can call super.init only inside a constructor");
      if (needDot())
        Builder.addLeadingDot();
      Builder.addTextChunk("init");
    }
    Builder.addLeftParen();
    addPatternParameters(Builder, CD->getArgParams());
    Builder.addRightParen();
    addTypeAnnotation(Builder, CD->getResultType());
  }

  void addSubscriptCall(const SubscriptDecl *SD, DeclVisibilityKind Reason) {
    assert(!HaveDot && "can not add a subscript after a dot");
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(SD, Reason));
    Builder.setAssociatedDecl(SD);
    Builder.addLeftBracket();
    addPatternParameters(Builder, SD->getIndices());
    Builder.addRightBracket();

    // Add a type annotation.
    Type T = SD->getElementType();
    if (IsDynamicLookup) {
      // Values of properties that were found on a DynamicLookup have
      // Optional<T> type.
      T = OptionalType::get(T, Ctx);
    }
    addTypeAnnotation(Builder, T);
  }

  void addNominalTypeRef(const NominalTypeDecl *NTD,
                         DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(NTD, Reason));
    Builder.setAssociatedDecl(NTD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(NTD->getName().str());
    addTypeAnnotation(Builder,
                      MetaTypeType::get(NTD->getDeclaredType(), Ctx));
  }

  void addTypeAliasRef(const TypeAliasDecl *TAD, DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Declaration,
        getSemanticContext(TAD, Reason));
    Builder.setAssociatedDecl(TAD);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(TAD->getName().str());
    if (TAD->hasUnderlyingType())
      addTypeAnnotation(Builder,
                        MetaTypeType::get(TAD->getUnderlyingType(), Ctx));
    else {
      addTypeAnnotation(Builder,
                        MetaTypeType::get(TAD->getDeclaredType(), Ctx));
    }
  }

  void addGenericTypeParamRef(const GenericTypeParamDecl *GP,
                              DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
      CompletionContext,
      CodeCompletionResult::ResultKind::Declaration,
      getSemanticContext(GP, Reason));
    Builder.setAssociatedDecl(GP);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(GP->getName().str());
    addTypeAnnotation(Builder,
                      MetaTypeType::get(GP->getDeclaredType(), Ctx));
  }

  void addAssociatedTypeRef(const AssociatedTypeDecl *AT,
                            DeclVisibilityKind Reason) {
    CodeCompletionResultBuilder Builder(
      CompletionContext,
      CodeCompletionResult::ResultKind::Declaration,
      getSemanticContext(AT, Reason));
    Builder.setAssociatedDecl(AT);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(AT->getName().str());
    addTypeAnnotation(Builder,
                      MetaTypeType::get(AT->getDeclaredType(), Ctx));
  }

  void addKeyword(StringRef Name, Type TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.isNull())
      addTypeAnnotation(Builder, TypeAnnotation);
  }

  void addKeyword(StringRef Name, StringRef TypeAnnotation) {
    CodeCompletionResultBuilder Builder(
        CompletionContext,
        CodeCompletionResult::ResultKind::Keyword,
        SemanticContextKind::None);
    if (needDot())
      Builder.addLeadingDot();
    Builder.addTextChunk(Name);
    if (!TypeAnnotation.empty())
      Builder.addTypeAnnotation(TypeAnnotation);
  }

  // Implement swift::VisibleDeclConsumer
  void foundDecl(ValueDecl *D, DeclVisibilityKind Reason) override {
    if (!D->hasType())
      TypeResolver->resolveDeclSignature(D);

    switch (Kind) {
    case LookupKind::ValueExpr:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        // Swift does not have class variables.
        // FIXME: add code completion results when class variables are added.
        assert(!ExprType->is<MetaTypeType>() && "name lookup bug");
        addVarDeclRef(VD, Reason);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call getters or setters.  We use VarDecls and
        // SubscriptDecls to produce completions that refer to getters and
        // setters.
        if (FD->isGetterOrSetter())
          return;

        addMethodCall(FD, Reason);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      if (auto *CD = dyn_cast<ConstructorDecl>(D)) {
        if (ExprType->is<MetaTypeType>()) {
          if (HaveDot)
            return;
          addConstructorCall(CD, Reason);
        }
        if (IsSuperRefExpr) {
          if (auto *AFD = dyn_cast<AbstractFunctionDecl>(CurrDeclContext))
            if (!isa<ConstructorDecl>(AFD))
              return;
          addConstructorCall(CD, Reason);
        }
        return;
      }

      if (HaveDot)
        return;

      if (auto *SD = dyn_cast<SubscriptDecl>(D)) {
        if (ExprType->is<MetaTypeType>())
          return;
        addSubscriptCall(SD, Reason);
        return;
      }
      return;

    case LookupKind::ValueInDeclContext:
      if (auto *VD = dyn_cast<VarDecl>(D)) {
        addVarDeclRef(VD, Reason);
        return;
      }

      if (auto *FD = dyn_cast<FuncDecl>(D)) {
        // We can not call operators with a postfix parenthesis syntax.
        if (FD->isBinaryOperator() || FD->isUnaryOperator())
          return;

        // We can not call getters or setters.  We use VarDecls and
        // SubscriptDecls to produce completions that refer to getters and
        // setters.
        if (FD->isGetterOrSetter())
          return;

        addMethodCall(FD, Reason);
        return;
      }

      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      return;

    case LookupKind::Type:
    case LookupKind::TypeInDeclContext:
      if (auto *NTD = dyn_cast<NominalTypeDecl>(D)) {
        addNominalTypeRef(NTD, Reason);
        return;
      }

      if (auto *TAD = dyn_cast<TypeAliasDecl>(D)) {
        addTypeAliasRef(TAD, Reason);
        return;
      }

      if (auto *GP = dyn_cast<GenericTypeParamDecl>(D)) {
        addGenericTypeParamRef(GP, Reason);
        return;
      }

      if (auto *AT = dyn_cast<AssociatedTypeDecl>(D)) {
        addAssociatedTypeRef(AT, Reason);
        return;
      }

      return;
    }
  }

  void tryAddStlibOptionalCompletions(Type ExprType) {
    // If there is a dot, we don't have any special completions for
    // Optional<T>.
    if (!needDot())
      return;

    ExprType = ExprType->getRValueType();
    Type Unwrapped = ExprType->getOptionalObjectType(Ctx);
    if (!Unwrapped)
      return;
    // FIXME: consider types convertible to T?.

    {
      CodeCompletionResultBuilder Builder(
          CompletionContext,
          CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::ExpressionSpecific);
      Builder.addExclamationMark();
      addTypeAnnotation(Builder, Unwrapped);
    }
    {
      CodeCompletionResultBuilder Builder(
          CompletionContext,
          CodeCompletionResult::ResultKind::Pattern,
          SemanticContextKind::ExpressionSpecific);
      Builder.addQuestionMark();
      addTypeAnnotation(Builder, Unwrapped);
    }
  }

  void getValueExprCompletions(Type ExprType) {
    Kind = LookupKind::ValueExpr;
    NeedLeadingDot = !HaveDot;
    this->ExprType = ExprType;
    bool Done = false;
    if (auto AFT = ExprType->getAs<AnyFunctionType>()) {
      addFunctionCall(AFT);
      Done = true;
    }
    if (auto LVT = ExprType->getAs<LValueType>()) {
      if (auto AFT = LVT->getObjectType()->getAs<AnyFunctionType>()) {
        addFunctionCall(AFT);
        Done = true;
      }
    }
    if (auto MT = ExprType->getAs<ModuleType>()) {
      if (auto *M = dyn_cast<ClangModule>(MT->getModule())) {
        CompletionContext.includeQualifiedClangResults(M, needDot());
        Done = true;
      }
    }
    tryAddStlibOptionalCompletions(ExprType);
    if (!Done) {
      lookupVisibleMemberDecls(*this, ExprType, CurrDeclContext,
                               TypeResolver.get());
    }
    {
      // Add the special qualified keyword 'metatype' so that, for example,
      // 'Int.metatype' can be completed.
      Type Annotation = ExprType;

      // First, unwrap the outer LValue.  LValueness of the expr is unrelated
      // to the LValueness of the metatype.
      if (auto *LVT = dyn_cast<LValueType>(Annotation.getPointer())) {
        Annotation = LVT->getObjectType();
      }

      Annotation = MetaTypeType::get(Annotation, Ctx);

      // Use the canonical type as a type annotation because looking at the
      // '.metatype' in the IDE is a way to understand what type the expression
      // has.
      addKeyword("metatype", Annotation->getCanonicalType());
    }
  }

  void getValueCompletionsInDeclContext(SourceLoc Loc) {
    Kind = LookupKind::ValueInDeclContext;
    lookupVisibleDecls(*this, CurrDeclContext, TypeResolver.get(), Loc);

    // FIXME: The pedantically correct way to find the type is to resolve the
    // swift.StringLiteralType type.
    addKeyword("__FILE__", "String");
    // Same: swift.IntegerLiteralType.
    addKeyword("__LINE__", "Int");
    addKeyword("__COLUMN__", "Int");

    if (Ctx.LangOpts.Axle)
      addAxleSugarTypeCompletions(/*metatypeAnnotation=*/true);

    CompletionContext.includeUnqualifiedClangResults();
  }

  void getTypeCompletions(Type BaseType) {
    Kind = LookupKind::Type;
    NeedLeadingDot = !HaveDot;
    lookupVisibleMemberDecls(*this, MetaTypeType::get(BaseType, Ctx),
                             CurrDeclContext, TypeResolver.get());
  }

  void getTypeCompletionsInDeclContext(SourceLoc Loc) {
    Kind = LookupKind::TypeInDeclContext;
    lookupVisibleDecls(*this, CurrDeclContext, TypeResolver.get(), Loc);

    if (Ctx.LangOpts.Axle)
      addAxleSugarTypeCompletions(/*metatypeAnnotation=*/false);
  }

private:
  void addAxleSugarTypeCompletions(bool metatypeAnnotation) {
    {
      // Vec<type, length>
      CodeCompletionResultBuilder Builder(
                                    CompletionContext,
                                    CodeCompletionResult::ResultKind::Pattern,
                                    SemanticContextKind::None);
      Builder.addTextChunk("Vec");
      Builder.addLeftAngle();
      Builder.addGenericParameter("type");
      Builder.addComma(", ");
      Builder.addGenericParameter("length");
      Builder.addRightAngle();

      if (metatypeAnnotation) {
        Builder.addTypeAnnotation("Vec<...>.metatype");
      }
    }

    {
      // Matrix<type, rows, columns>
      CodeCompletionResultBuilder Builder(
                                    CompletionContext,
                                    CodeCompletionResult::ResultKind::Pattern,
                                    SemanticContextKind::None);
      Builder.addTextChunk("Matrix");
      Builder.addLeftAngle();
      Builder.addGenericParameter("type");
      Builder.addComma(", ");
      Builder.addGenericParameter("rows");
      // FIXME: Make this a default argument.
      Builder.addComma(", ");
      Builder.addGenericParameter("columns");
      Builder.addRightAngle();

      if (metatypeAnnotation) {
        Builder.addTypeAnnotation("Matrix<...>.metatype");
      }
    }

  }
};

} // end unnamed namespace

void CodeCompletionCallbacksImpl::completeDotExpr(Expr *E) {
  // Don't produce any results in an enum case.
  if (InEnumCaseRawValue)
    return;

  Kind = CompletionKind::DotExpr;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completePostfixExprBeginning() {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum case.
  if (InEnumCaseRawValue)
    return;

  Kind = CompletionKind::PostfixExprBeginning;
  CurDeclContext = P.CurDeclContext;
  CStyleForLoopIterationVariable =
      CodeCompletionCallbacks::CStyleForLoopIterationVariable;
}

void CodeCompletionCallbacksImpl::completePostfixExpr(Expr *E) {
  assert(P.Tok.is(tok::code_complete));

  // Don't produce any results in an enum case.
  if (InEnumCaseRawValue)
    return;

  Kind = CompletionKind::PostfixExpr;
  ParsedExpr = E;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeExprSuper(SuperRefExpr *SRE) {
  // Don't produce any results in an enum case.
  if (InEnumCaseRawValue)
    return;

  Kind = CompletionKind::SuperExpr;
  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeExprSuperDot(SuperRefExpr *SRE) {
  // Don't produce any results in an enum case.
  if (InEnumCaseRawValue)
    return;

  Kind = CompletionKind::SuperExprDot;
  ParsedExpr = SRE;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeSimpleBeginning() {
  Kind = CompletionKind::TypeSimpleBeginning;
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithDot(
    IdentTypeRepr *ITR) {
  if (!ITR) {
    completeTypeSimpleBeginning();
    return;
  }
  Kind = CompletionKind::TypeIdentifierWithDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

void CodeCompletionCallbacksImpl::completeTypeIdentifierWithoutDot(
    IdentTypeRepr *ITR) {
  assert(ITR);
  Kind = CompletionKind::TypeIdentifierWithoutDot;
  ParsedTypeLoc = TypeLoc(ITR);
  CurDeclContext = P.CurDeclContext;
}

static bool isDynamicLookup(Type T) {
  if (auto *PT = T->getRValueType()->getAs<ProtocolType>())
    return PT->getDecl()->isSpecificProtocol(KnownProtocolKind::DynamicLookup);
  return false;
}

void CodeCompletionCallbacksImpl::doneParsing() {
  if (Kind == CompletionKind::None) {
    DEBUG(llvm::dbgs() << "did not get a completion callback");
    return;
  }

  if (!typecheckContext())
    return;

  if (DelayedParsedDecl && !typecheckDelayedParsedDecl())
    return;

  if (auto *AFD = dyn_cast_or_null<AbstractFunctionDecl>(DelayedParsedDecl))
    CurDeclContext = AFD;

  if (ParsedExpr && !typecheckParsedExpr())
    return;

  if (!ParsedTypeLoc.isNull() && !typecheckParsedType())
    return;

  CompletionLookup Lookup(CompletionContext, P.Context, CurDeclContext);

  switch (Kind) {
  case CompletionKind::None:
    llvm_unreachable("should be already handled");
    return;

  case CompletionKind::DotExpr: {
    Lookup.setHaveDot();
    Type ExprType = ParsedExpr->getType();
    if (isDynamicLookup(ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(ExprType);
    break;
  }

  case CompletionKind::PostfixExprBeginning: {
    if (CStyleForLoopIterationVariable)
      Lookup.addExpressionSpecificDecl(CStyleForLoopIterationVariable);
    Lookup.getValueCompletionsInDeclContext(
        P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }

  case CompletionKind::PostfixExpr: {
    Type ExprType = ParsedExpr->getType();
    if (isDynamicLookup(ExprType))
      Lookup.setIsDynamicLookup();
    Lookup.getValueExprCompletions(ExprType);
    break;
  }

  case CompletionKind::SuperExpr: {
    Lookup.setIsSuperRefExpr();
    Lookup.getValueExprCompletions(ParsedExpr->getType());
    break;
  }

  case CompletionKind::SuperExprDot: {
    Lookup.setIsSuperRefExpr();
    Lookup.setHaveDot();
    Lookup.getValueExprCompletions(ParsedExpr->getType());
    break;
  }

  case CompletionKind::TypeSimpleBeginning: {
    Lookup.getTypeCompletionsInDeclContext(
        P.Context.SourceMgr.getCodeCompletionLoc());
    break;
  }

  case CompletionKind::TypeIdentifierWithDot: {
    Lookup.setHaveDot();
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }

  case CompletionKind::TypeIdentifierWithoutDot: {
    Lookup.getTypeCompletions(ParsedTypeLoc.getType());
    break;
  }
  }

  deliverCompletionResults();
}

void CodeCompletionCallbacksImpl::deliverCompletionResults() {
  auto Results = CompletionContext.takeResults();
  if (!Results.empty()) {
    Consumer.handleResults(Results);
    DeliveredResults = true;
  }
}

void CodeCompletionCallbacksImpl::loadedModule(ModuleLoader *Loader,
                                               Module *M) {
  CompletionContext.clearClangCache();
}

void PrintingCodeCompletionConsumer::handleResults(
    MutableArrayRef<CodeCompletionResult *> Results) {
  OS << "Begin completions, " << Results.size() << " items\n";
  for (auto Result : Results) {
    Result->print(OS);
    OS << "\n";
  }
  OS << "End completions\n";
}

namespace {
class CodeCompletionCallbacksFactoryImpl
    : public CodeCompletionCallbacksFactory {
  CodeCompletionContext &CompletionContext;
  CodeCompletionConsumer &Consumer;

public:
  CodeCompletionCallbacksFactoryImpl(CodeCompletionContext &CompletionContext,
                                     CodeCompletionConsumer &Consumer)
      : CompletionContext(CompletionContext), Consumer(Consumer) {}

  CodeCompletionCallbacks *createCodeCompletionCallbacks(Parser &P) override {
    return new CodeCompletionCallbacksImpl(P, CompletionContext, Consumer);
  }
};
} // end unnamed namespace

CodeCompletionCallbacksFactory *
swift::ide::makeCodeCompletionCallbacksFactory(
    CodeCompletionContext &CompletionContext,
    CodeCompletionConsumer &Consumer) {
  return new CodeCompletionCallbacksFactoryImpl(CompletionContext, Consumer);
}

