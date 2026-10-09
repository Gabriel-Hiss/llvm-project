//===- FrontendContextStub.cpp - test-only FrontendContext --------*- C++ -*-===//
//
// Minimal definitions of the non-inline FrontendContext members (owned by
// Swe2ClangCore, lib/Core/) so the lib/Type unit tests link standalone.
// Behavior matches the header contract: ident() sanitizes and caches,
// functionDecl/globalDecl create declaration-only decls on first use.
// This file is test-only and never ships in the frontend library.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/SmallVector.h"

namespace revenant::clangx {

clang::IdentifierInfo *FrontendContext::ident(llvm::StringRef Name) {
  std::string Clean = sanitizeIdentifier(Name, isCPlusPlus());
  auto It = Sanitized.find(Clean);
  if (It != Sanitized.end())
    return It->second;
  clang::IdentifierInfo *II = &ast().Idents.get(Clean);
  Sanitized[Clean] = II;
  return II;
}

clang::FunctionDecl *FrontendContext::functionDecl(llvm::Function &F,
                                                     TypeBuilder &Types) {
  if (auto *D = llvm::dyn_cast_or_null<clang::FunctionDecl>(lookupDecl(&F)))
    return D;
  clang::QualType QT = Types.typeOf(F.getFunctionType(), nullptr);
  const auto *FPT = QT->getAs<clang::FunctionProtoType>();
  llvm::SmallVector<clang::QualType, 8> Params;
  if (FPT)
    for (unsigned I = 0; I < FPT->getNumParams(); ++I) Params.push_back(FPT->getParamType(I));
  clang::QualType Ret =
      FPT ? FPT->getReturnType() : ast().UnsignedLongLongTy;
  clang::FunctionProtoType::ExtProtoInfo EPI;
  clang::QualType Fn = ast().getFunctionType(Ret, Params, EPI);
  auto *FD = clang::FunctionDecl::Create(
      ast(), ast().getTranslationUnitDecl(), clang::SourceLocation(),
      clang::SourceLocation(), ident(F.getName()), Fn,
      ast().getTrivialTypeSourceInfo(Fn), clang::SC_None);
  llvm::SmallVector<clang::ParmVarDecl *, 8> Ps;
  unsigned I = 0;
  for (llvm::Argument &A : F.args()) {
    clang::QualType PT =
        I < Params.size() ? Params[I] : ast().UnsignedLongLongTy;
    Ps.push_back(clang::ParmVarDecl::Create(
        ast(), FD, clang::SourceLocation(), clang::SourceLocation(),
        ident("a" + std::to_string(I + 1)), PT,
        ast().getTrivialTypeSourceInfo(PT), clang::SC_None, nullptr));
    ++I;
  }
  FD->setParams(Ps);
  bindDecl(&F, FD);
  return FD;
}

clang::VarDecl *FrontendContext::globalDecl(llvm::GlobalVariable &GV,
                                              TypeBuilder &Types) {
  if (auto *D = llvm::dyn_cast_or_null<clang::VarDecl>(lookupDecl(&GV)))
    return D;
  clang::QualType QT = Types.globalType(GV);
  auto *VD = clang::VarDecl::Create(
      ast(), ast().getTranslationUnitDecl(), clang::SourceLocation(),
      clang::SourceLocation(), ident(GV.getName()), QT,
      ast().getTrivialTypeSourceInfo(QT), clang::SC_None);
  bindDecl(&GV, VD);
  return VD;
}

SourceLang moduleLang(const llvm::Module &M) {
  if (const auto *Flag = llvm::cast_or_null<llvm::MDString>(
          M.getModuleFlag("revenant.lang"))) {
    if (Flag->getString() == "cpp")
      return SourceLang::CPlusPlus;
  }
  return SourceLang::C;
}

bool isPlainIdentifier(llvm::StringRef Name) {
  if (Name.empty() || !(llvm::isAlpha(Name.front()) || Name.front() == '_'))
    return false;
  return llvm::all_of(Name.drop_front(), [](char C) {
    return llvm::isAlnum(C) || C == '_' || C == '$';
  });
}

std::string sanitizeIdentifier(llvm::StringRef Name, bool CPlusPlus) {
  (void)CPlusPlus;
  if (isPlainIdentifier(Name))
    return std::string(Name);
  std::string Out;
  for (char C : Name) {
    if (llvm::isAlnum(C) || C == '_' || C == '$')
      Out.push_back(C);
    else
      Out.push_back('_');
  }
  if (Out.empty() || llvm::isDigit(Out.front()))
    Out = "_" + Out;
  return Out;
}

} // namespace revenant::clangx
