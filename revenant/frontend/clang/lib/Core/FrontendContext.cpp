//===- FrontendContext.cpp - shared Clang frontend state --------*- C++ -*-===//

#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/TypeBuilder.h"
#include "clang/AST/Decl.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/Metadata.h"
#include <cctype>

using llvm::StringRef;
using llvm::Twine;

namespace revenant::clangx {

SourceLang moduleLang(const llvm::Module &M) {
  if (auto *S = llvm::dyn_cast_or_null<llvm::MDString>(
          M.getModuleFlag("revenant.lang"))) {
    StringRef V = S->getString();
    if (V == "cpp" || V == "c++" || V == "cxx")
      return SourceLang::CPlusPlus;
    return SourceLang::C;
  }
  return SourceLang::C;
}

static bool isKeyword(StringRef Name, bool CPlusPlus) {
  static const char *CKeys[] = {
      "auto",       "break",    "case",     "char",    "const",
      "continue",   "default",  "do",       "double",  "else",
      "enum",       "extern",   "float",    "for",     "goto",
      "if",         "inline",   "int",      "long",    "register",
      "restrict",   "return",   "short",    "signed",  "sizeof",
      "static",     "struct",   "switch",   "typedef", "union",
      "unsigned",   "void",     "volatile", "while",   "_Bool",
      "_Complex",   "_Alignas", "_Alignof", "_Atomic", "_Static_assert",
      "_Noreturn",  "_Thread_local", "_Generic", "_Imaginary",
  };
  static const char *CXXKeys[] = {
      "alignas",        "alignof",   "asm",        "bool",
      "catch",          "char16_t",  "char32_t",   "char8_t",
      "class",          "concept",   "consteval",  "constexpr",
      "constinit",      "const_cast", "co_await",  "co_return",
      "co_yield",       "decltype",  "delete",     "dynamic_cast",
      "explicit",       "export",    "false",      "friend",
      "mutable",        "namespace", "new",        "noexcept",
      "nullptr",        "operator",  "private",    "protected",
      "public",         "reinterpret_cast", "requires", "static_assert",
      "static_cast",    "template",  "this",       "thread_local",
      "throw",          "true",      "try",        "typeid",
      "typename",       "using",     "virtual",    "wchar_t",
  };
  for (const char *K : CKeys)
    if (Name == K)
      return true;
  if (CPlusPlus)
    for (const char *K : CXXKeys)
      if (Name == K)
        return true;
  return false;
}

bool isPlainIdentifier(StringRef Name) {
  if (Name.empty())
    return false;
  auto OK = [](char C, bool First) {
    if (std::isalpha((unsigned char)C) || C == '_')
      return true;
    return !First && (std::isdigit((unsigned char)C) || C == '$');
  };
  if (!OK(Name.front(), true))
    return false;
  for (char C : Name.drop_front())
    if (!OK(C, false))
      return false;
  return true;
}

std::string sanitizeIdentifier(StringRef Name, bool CPlusPlus) {
  std::string Out;
  Out.reserve(Name.size() + 8);
  for (char C : Name) {
    if (std::isalnum((unsigned char)C) || C == '_' || C == '$') {
      Out.push_back(C);
    } else {
      char Buf[4];
      std::snprintf(Buf, sizeof(Buf), "_%02x", (unsigned char)C);
      Out += Buf;
    }
  }
  if (Out.empty() || std::isdigit((unsigned char)Out.front()))
    Out.insert(Out.begin(), '_');
  if (isKeyword(Out, CPlusPlus))
    Out.push_back('_');
  return Out;
}

void FrontendContext::requireInclude(StringRef Header) {
  for (const std::string &Existing : RequestedIncludes)
    if (Existing == Header)
      return;
  RequestedIncludes.emplace_back(Header.str());
}

clang::IdentifierInfo *FrontendContext::ident(StringRef Name) {
  std::string S = Name.str();
  if (!isPlainIdentifier(S) || isKeyword(S, isCPlusPlus()))
    S = sanitizeIdentifier(S, isCPlusPlus());
  auto It = Sanitized.find(S);
  if (It != Sanitized.end())
    return It->second;
  clang::IdentifierInfo *II = &AST.Idents.get(S);
  Sanitized[S] = II;
  return II;
}

clang::FunctionDecl *
FrontendContext::functionDecl(llvm::Function &F, TypeBuilder &Types) {
  if (auto *D = llvm::dyn_cast_or_null<clang::FunctionDecl>(lookupDecl(&F)))
    return D;
  clang::QualType FT = Types.functionType(F);
  // Normalize main/wmain signature for MSVC/clang-cl compatibility.
  // Keep the lifted arity (argc/argv, plus envp when recovered): only the
  // spellings are normalized, so the decl always matches the definition.
  // The lifted IR may carry fewer arguments (e.g. argc-only `i32 @main(i32)`);
  // buildFunction pads ParmVarDecls to the declared arity below.
  if (F.getName() == "main" || F.getName() == "wmain") {
    clang::ASTContext &A = AST;
    clang::QualType IntTy = A.IntTy;
    clang::QualType CharPtrTy = A.getPointerType(A.CharTy);
    clang::QualType CharPtrPtrTy = A.getPointerType(CharPtrTy);
    llvm::SmallVector<clang::QualType, 4> Params;
    for (size_t I = 0; I < F.arg_size(); ++I)
      Params.push_back(I == 0 ? IntTy : CharPtrPtrTy);
    FT = A.getFunctionType(IntTy, Params, clang::FunctionProtoType::ExtProtoInfo());
  }
  auto *FD = clang::FunctionDecl::Create(
      AST, AST.getTranslationUnitDecl(), clang::SourceLocation(),
      clang::SourceLocation(),
      clang::DeclarationName(ident(F.getName())), FT, /*TInfo=*/nullptr,
      clang::SC_None);
  AST.getTranslationUnitDecl()->addDecl(FD);
  bindDecl(&F, FD);
  return FD;
}

clang::VarDecl *
FrontendContext::globalDecl(llvm::GlobalVariable &GV, TypeBuilder &Types) {
  if (auto *D = llvm::dyn_cast_or_null<clang::VarDecl>(lookupDecl(&GV)))
    return D;
  clang::QualType T = Types.globalType(GV);
  auto *VD = clang::VarDecl::Create(
      AST, AST.getTranslationUnitDecl(), clang::SourceLocation(),
      clang::SourceLocation(), ident(GV.getName()), T, nullptr,
      GV.isDeclaration() ? clang::SC_Extern : clang::SC_None);
  AST.getTranslationUnitDecl()->addDecl(VD);
  bindDecl(&GV, VD);
  return VD;
}

} // namespace revenant::clangx
