//===- FrontendContextTestShim.cpp - test-only FrontendContext --*- C++ -*-===//
//
// Minimal definitions of the FrontendContext methods lib/Expr calls, so
// the unit tests link before lib/Core lands. Core owns the real
// implementations; when they land, this file is deleted and the tests
// link the Core library instead.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"

namespace revenant::clangx {
using namespace clang;

IdentifierInfo *FrontendContext::ident(llvm::StringRef Name) {
  std::string S = sanitizeIdentifier(Name, isCPlusPlus());
  auto It = Sanitized.find(S);
  if (It != Sanitized.end())
    return It->second;
  IdentifierInfo *II = &ast().Idents.get(S);
  Sanitized[S] = II;
  return II;
}

FunctionDecl *FrontendContext::functionDecl(
    llvm::Function &F,
    llvm::function_ref<QualType(llvm::Type *, const llvm::DIType *)> typeOf) {
  if (ValueDecl *D = lookupDecl(&F))
    return cast<FunctionDecl>(D);
  // Build a declaration-only prototype from the IR signature.
  ASTContext &A = ast();
  SmallVector<QualType, 8> Params;
  for (llvm::Argument &Arg : F.args())
    Params.push_back(typeOf(Arg.getType(), nullptr));
  llvm::FunctionType *FT = F.getFunctionType();
  QualType Ret = typeOf(FT->getReturnType(), nullptr);
  FunctionProtoType::ExtProtoInfo EPI;
  QualType FQT = A.getFunctionType(Ret, Params, EPI);
  IdentifierInfo *Id = ident(F.getName());
  FunctionDecl *FD = FunctionDecl::Create(
      A, A.getTranslationUnitDecl(), SourceLocation(), SourceLocation(), Id,
      FQT, A.getTrivialTypeSourceInfo(FQT), SC_Extern);
  SmallVector<ParmVarDecl *, 8> Ps;
  unsigned Idx = 0;
  for (llvm::Argument &Arg : F.args()) {
    ParmVarDecl *P = ParmVarDecl::Create(
        A, FD, SourceLocation(), SourceLocation(),
        ident(Arg.getName().empty()
                  ? ("a" + std::to_string(Idx + 1))
                  : Arg.getName()),
        Params[Idx], A.getTrivialTypeSourceInfo(Params[Idx]), SC_None,
        nullptr);
    Ps.push_back(P);
    bindDecl(&Arg, P);
    ++Idx;
  }
  FD->setParams(Ps);
  bindDecl(&F, FD);
  return FD;
}

VarDecl *FrontendContext::globalDecl(
    llvm::GlobalVariable &GV,
    llvm::function_ref<QualType(llvm::Type *, const llvm::DIType *)> typeOf) {
  if (ValueDecl *D = lookupDecl(&GV))
    return cast<VarDecl>(D);
  ASTContext &A = ast();
  QualType T = typeOf(GV.getValueType(), nullptr);
  VarDecl *VD = VarDecl::Create(A, A.getTranslationUnitDecl(),
                                SourceLocation(), SourceLocation(),
                                ident(GV.getName()), T,
                                A.getTrivialTypeSourceInfo(T), SC_None);
  bindDecl(&GV, VD);
  return VD;
}

SourceLang moduleLang(const llvm::Module &M) {
  if (llvm::Metadata *Op = M.getModuleFlag("revenant.lang")) {
    if (auto *S = llvm::dyn_cast_or_null<llvm::MDString>(Op)) {
      if (S->getString() == "cpp" || S->getString() == "c++")
        return SourceLang::CPlusPlus;
    }
  }
  return SourceLang::C;
}

bool isPlainIdentifier(llvm::StringRef Name) {
  if (Name.empty() ||
      (!llvm::isAlpha(Name[0]) && Name[0] != '_' && Name[0] != '$'))
    return false;
  for (char C : Name)
    if (!llvm::isAlnum(C) && C != '_' && C != '$')
      return false;
  return true;
}

std::string sanitizeIdentifier(llvm::StringRef Name, bool CPlusPlus) {
  std::string S = Name.str();
  for (char &C : S)
    if (!llvm::isAlnum(C) && C != '_' && C != '$')
      C = '_';
  if (S.empty() || (!llvm::isAlpha(S[0]) && S[0] != '_' && S[0] != '$'))
    S = "_" + S;
  // Keep the sanitized spelling stable for the C++ keyword set.
  static const char *Keywords[] = {
      "class", "template", "typename", "namespace", "virtual", "this",
      "new",   "delete",   "operator", "friend",    "public",  "private",
      "protected", "constexpr", "decltype", "noexcept", "static_assert",
  };
  for (const char *K : Keywords)
    if (S == K) {
      S += "_";
      break;
    }
  (void)CPlusPlus;
  return S;
}

} // namespace revenant::clangx
