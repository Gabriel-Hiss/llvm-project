//==- tools/revenant-libdb/revenant-libdb.cpp -----------------------------==//
//
// Build a relocation-masked database from static archives, match an analyzed
// PE, and report exact matches. Database JSON is intentionally inspectable.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Analysis.h"
#include "revenant/Analysis/LibMatch.h"
#include "revenant/Analysis/Signatures.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"
#include "revenant/Target/TargetHooks.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "llvm/ADT/StringExtras.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Object/Archive.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/PDBSymbolFunc.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::object;
using namespace revenant;

static cl::opt<std::string> Mode(cl::Positional, cl::Required,
                                 cl::desc("build or match"));
static cl::opt<std::string> Output("o", cl::Required,
                                   cl::desc("database or match CSV path"));
static cl::opt<std::string> TargetBinary("binary",
                                        cl::desc("input PE to match"));
static cl::opt<std::string> PdbPath("pdb", cl::desc("optional PDB for validation"));
static cl::list<std::string> Inputs(cl::Positional, cl::ZeroOrMore,
                                    cl::desc("static archives"));
static cl::list<std::string> CHeaders(
    "c-header", cl::desc("C SDK/UCRT header to parse"));
static cl::list<std::string> CxxHeaders(
    "cxx-header", cl::desc("C++ library header to parse"));
static cl::list<std::string> IncludeDirs(
    "include-dir", cl::desc("system include directory for header parsing"));

static std::map<std::string, LibraryPrototype> Prototypes;
class PrototypeVisitor final
    : public clang::RecursiveASTVisitor<PrototypeVisitor> {
public:
  explicit PrototypeVisitor(clang::ASTContext &Context) : Ctx(Context) {}

  bool VisitFunctionDecl(clang::FunctionDecl *FD) {
    if (FD->isImplicit() || FD->getName().empty() ||
        !FD->getDeclName().isIdentifier())
      return true;
    std::string Mangled;
    raw_string_ostream OS(Mangled);
    clang::MangleContext *MC = Ctx.createMangleContext();
    MC->mangleName(clang::GlobalDecl(FD->getCanonicalDecl()), OS);
    OS.flush();
    LibraryPrototype Proto;
    if (const auto *Method = llvm::dyn_cast<clang::CXXMethodDecl>(FD))
      if (Method->isInstance())
        Proto.Params.push_back(
            {"this", Method->getThisType().getAsString(),
             static_cast<uint32_t>(Ctx.getTypeSize(Method->getThisType())),
             false});
    Proto.ReturnType = FD->getReturnType().getAsString();
    Proto.ReturnVoid = FD->getReturnType()->isVoidType();
    Proto.ReturnFloat = FD->getReturnType()->isRealFloatingType();
    Proto.ReturnBits = Proto.ReturnVoid
                           ? 0
                           : static_cast<uint32_t>(
                                 Ctx.getTypeSize(FD->getReturnType()));
    Proto.VarArgs = FD->isVariadic();
    for (const clang::ParmVarDecl *Param : FD->parameters()) {
      LibraryParam P;
      P.Name = Param->getNameAsString();
      P.Type = Param->getType().getAsString();
      P.Bits = static_cast<uint32_t>(Ctx.getTypeSize(Param->getType()));
      P.Float = Param->getType()->isRealFloatingType();
      Proto.Params.push_back(std::move(P));
    }
    Prototypes.insert_or_assign(Mangled, Proto);
    if (FD->isExternC())
      Prototypes.insert_or_assign(FD->getNameAsString(), std::move(Proto));
    return true;
  }

  clang::ASTContext &Ctx;
};
class PrototypeConsumer final : public clang::ASTConsumer {
public:
  void HandleTranslationUnit(clang::ASTContext &Context) override {
    PrototypeVisitor Visitor(Context);
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }
};

class PrototypeAction final : public clang::ASTFrontendAction {
public:
  std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &, StringRef) override {
    return std::make_unique<PrototypeConsumer>();
  }
};
static Error parseHeader(StringRef Path, bool IsCXX) {
  auto Invocation = std::make_shared<clang::CompilerInvocation>();
  clang::Language Lang =
      IsCXX ? clang::Language::CXX : clang::Language::C;
  Invocation->getFrontendOpts().Inputs.push_back(
      clang::FrontendInputFile(Path, clang::InputKind(Lang)));
  Invocation->getFrontendOpts().ProgramAction = clang::frontend::ParseSyntaxOnly;
  Invocation->getTargetOpts().Triple = "x86_64-pc-windows-msvc";
  Invocation->getLangOpts().LangStd =
      IsCXX ? clang::LangStandard::lang_cxx17 : clang::LangStandard::lang_c17;
  Invocation->getLangOpts().CPlusPlus = IsCXX;
  Invocation->getLangOpts().MicrosoftExt = true;
  Invocation->getLangOpts().MSVCCompat = true;
  Invocation->getLangOpts().DeclSpecKeyword = true;
  for (const std::string &Dir : IncludeDirs)
    Invocation->getHeaderSearchOpts().AddPath(
        Dir, clang::frontend::System, false, false);
  clang::CompilerInstance Compiler(std::move(Invocation));
  Compiler.setVirtualFileSystem(vfs::getRealFileSystem());
  Compiler.createDiagnostics();
  PrototypeAction Action;
  if (!Compiler.ExecuteAction(Action))
    return createStringError(inconvertibleErrorCode(),
                             "cannot parse prototype header %s",
                             Path.str().c_str());
  return Error::success();
}

/// The `name` portion of a demangled spelling, mirroring the Rust
/// `demangle().short` contract: parameter list plus return/access/call
/// decoration dropped, rust legacy hash suffix stripped. Plain names pass
/// through unchanged.
/// Index of the last top-level `(` in a demangled spelling — the start of
/// the parameter list. Top-level means outside `<>` template brackets and
/// outside `` `name' `` scopes; `operator()`/`operator<` keep their
/// bracket by guarding on the `operator` token that precedes it.
static size_t paramListPos(StringRef Full) {
  int Depth = 0;
  size_t Params = StringRef::npos;
  for (size_t I = 0; I < Full.size(); ++I) {
    char C = Full[I];
    if (C == '`') {
      size_t Close = Full.find('\'', I + 1);
      if (Close != StringRef::npos) {
        I = Close;
        continue;
      }
    }
    if (C == '(' && Depth == 0 &&
        !(I >= 8 && Full.substr(I - 8, 8) == "operator"))
      Params = I;
    else if (C == '<') {
      size_t Tok = Full.find_last_of(": ", I);
      if (Full.substr(Tok == StringRef::npos ? 0 : Tok + 1,
                      I - (Tok == StringRef::npos ? 0 : Tok + 1)) !=
          "operator")
        ++Depth;
    } else if (C == '>' && Depth > 0)
      --Depth;
  }
  return Params;
}

static std::string shortName(StringRef Full) {
  size_t Params = paramListPos(Full);
  StringRef Head =
      Full.take_front(Params == StringRef::npos ? Full.size() : Params).trim();

  // MSVC spelling decorates the head: `public:`, `[thunk]:`, `static`,
  // the return type, and a `__cdecl`-family calling convention. The
  // convention is the last decoration before the name; cut everything up
  // to and including it. A convention inside `<>` is a function-pointer
  // template argument and does not count.
  int Depth = 0;
  size_t Conv = StringRef::npos;
  for (size_t I = 0; I < Head.size(); ++I) {
    char C = Head[I];
    if (C == '`') {
      size_t Close = Head.find('\'', I + 1);
      if (Close != StringRef::npos) {
        I = Close;
        continue;
      }
    }
    if (C == '<')
      ++Depth;
    else if (C == '>' && Depth > 0)
      --Depth;
    else if (C == '_' && Depth == 0 && I > 0 && Head[I - 1] == ' ' &&
             (Head.substr(I).starts_with("__cdecl") ||
              Head.substr(I).starts_with("__stdcall") ||
              Head.substr(I).starts_with("__fastcall") ||
              Head.substr(I).starts_with("__thiscall") ||
              Head.substr(I).starts_with("__vectorcall") ||
              Head.substr(I).starts_with("__clrcall")))
      Conv = I;
  }
  if (Conv != StringRef::npos) {
    size_t NameStart = Head.find(' ', Conv);
    Head = NameStart == StringRef::npos ? StringRef()
                                        : Head.drop_front(NameStart + 1);
  }
  for (StringRef Prefix : {"public:", "private:", "protected:", "static ",
                           "virtual ", "[thunk]:"})
    if (Head.starts_with(Prefix))
      Head = Head.drop_front(Prefix.size()).trim();

  // rustc-demangle renders legacy symbols as `path::name::h<16 hex>`;
  // strip the hash tail like the Rust oracle does.
  size_t Hash = Head.rfind("::h");
  if (Hash != StringRef::npos) {
    StringRef Tail = Head.drop_front(Hash + 3);
    if (Tail.size() == 16 &&
        llvm::all_of(Tail, [](char C) { return isHexDigit(C); }))
      Head = Head.take_front(Hash);
  }
  // Backtick scopes can carry a full signature (the quoted enclosing
  // function of a funclet/lambda). Shorten each such segment in place.
  std::string Quoted;
  for (size_t I = 0; I < Head.size();) {
    if (Head[I] != '`') {
      Quoted.push_back(Head[I++]);
      continue;
    }
    size_t Close = Head.find('\'', I + 1);
    if (Close == StringRef::npos) {
      Quoted += Head.substr(I);
      break;
    }
    StringRef Inner = Head.slice(I + 1, Close);
    if (Inner.contains('(')) {
      std::string Short = shortName(Inner);
      Quoted += '`';
      Quoted += Short.empty() ? Inner.str() : std::move(Short);
      Quoted += '\'';
    } else {
      Quoted += Head.slice(I, Close + 1);
    }
    I = Close + 1;
  }
  Head = Quoted;

  return Head.str();
}

/// Comparison key for the PDB correctness check: alphanumeric runs only,
/// `class`/`struct`/`enum`/`union`/`const` qualifiers erased at token
/// boundaries, and `dtor`/`destructor` spellings unified — so
/// `closure$0`, `{closure#0}` and `F<class H>` vs `F<H>` spellings of the
/// same symbol compare equal.
static std::string normKey(StringRef Name) {
  std::string Key;
  for (size_t I = 0; I < Name.size();) {
    bool Boundary = I == 0 || !(isAlnum(Name[I - 1]) || Name[I - 1] == '_');
    bool Erased = false;
    if (Boundary)
      for (StringRef Kw : {"class", "struct", "enum", "union", "const"})
        if (Name.substr(I).starts_with(Kw) &&
            (I + Kw.size() >= Name.size() ||
             !(isAlnum(Name[I + Kw.size()]) || Name[I + Kw.size()] == '_'))) {
          I += Kw.size();
          Erased = true;
          break;
        }
    if (Erased)
      continue;
    if (isAlnum(Name[I]) || Name[I] == '_')
      Key.push_back(Name[I]);
    ++I;
  }
  // DIA spells `deleting destructor`, llvm demangles `deleting dtor`.
  for (StringRef Alias : {"deletingdtor", "vbasedtor"}) {
    size_t Pos = Key.find(Alias);
    if (Pos != std::string::npos)
      Key.replace(Pos, Alias.size(),
                  Alias == "deletingdtor" ? "deletingdestructor"
                                          : "vbasedestructor");
  }
  return Key;
}

/// Win64 register-slot width for a demangled parameter type. Everything
/// non-scalar (pointers, references, aggregates by value) takes one
/// 64-bit slot; only scalar spelling narrows the hint.
static uint32_t slotBits(StringRef Type) {
  Type = Type.trim();
  if (Type.contains("char") || Type.contains("bool"))
    return 8;
  if (Type.contains("short") || Type.contains("wchar_t"))
    return 16;
  if (Type.contains("float"))
    return 32;
  if (Type.contains("double"))
    return 64;
  if (Type.contains("long long") || Type.contains("__int64"))
    return 64;
  if (Type.contains("long") || Type.contains("int") || Type.contains("enum"))
    return 32;
  return 64;
}

/// Split `S` at `Delim` occurrences that sit outside `<>`, `()`, `[]` and
/// `` `name' `` scopes.
static std::vector<StringRef> splitTopLevel(StringRef S, char Delim) {
  std::vector<StringRef> Out;
  int Depth = 0;
  size_t Start = 0;
  for (size_t I = 0; I < S.size(); ++I) {
    char C = S[I];
    if (C == '`') {
      size_t Close = S.find('\'', I + 1);
      if (Close != StringRef::npos) {
        I = Close;
        continue;
      }
    }
    if (C == '<' || C == '(' || C == '[')
      ++Depth;
    else if ((C == '>' || C == ')' || C == ']') && Depth > 0)
      --Depth;
    else if (C == Delim && Depth == 0) {
      Out.push_back(S.slice(Start, I).trim());
      Start = I + 1;
    }
  }
  Out.push_back(S.drop_front(Start).trim());
  return Out;
}

/// Recover a prototype from the demangled spelling of a mangled record.
/// MSVC `?`-names encode the full signature; the parameter list comes back
/// verbatim and the return type is everything before the calling
/// convention. Itanium `_Z` names encode parameters but no return type
/// (reported as an unknown 64-bit return). Plain and Rust-v0 spellings
/// carry no signature and yield nothing.
static std::optional<LibraryPrototype>
prototypeFromDemangled(StringRef Demangled) {
  size_t Open = paramListPos(Demangled);
  size_t Close = Demangled.rfind(')');
  if (Open == StringRef::npos || Close == StringRef::npos || Close <= Open)
    return std::nullopt;

  LibraryPrototype Proto;
  StringRef Params = Demangled.slice(Open + 1, Close).trim();
  if (!Params.empty() && Params != "void")
    for (StringRef P : splitTopLevel(Params, ',')) {
      if (P == "...") {
        Proto.VarArgs = true;
        continue;
      }
      LibraryParam Param;
      Param.Type = P.str();
      Param.Bits = slotBits(P);
      Param.Float = (P.contains("float") || P.contains("double")) &&
                    !P.contains('*') && !P.contains('&') &&
                    !P.contains('(') && !P.contains('[');
      Proto.Params.push_back(std::move(Param));
    }

  // The head before `(` is `RET __cdecl NAME` for MSVC spellings: the
  // last top-level convention keyword separates the two. No convention
  // means an Itanium spelling — parameters recovered, return unknown.
  StringRef Head = Demangled.take_front(Open).trim();
  int Depth = 0;
  size_t Conv = StringRef::npos;
  for (size_t I = 0; I < Head.size(); ++I) {
    char C = Head[I];
    if (C == '`') {
      size_t Tick = Head.find('\'', I + 1);
      if (Tick != StringRef::npos) {
        I = Tick;
        continue;
      }
    }
    if (C == '<')
      ++Depth;
    else if (C == '>' && Depth > 0)
      --Depth;
    else if (C == '_' && Depth == 0 && I > 0 && Head[I - 1] == ' ' &&
             (Head.substr(I).starts_with("__cdecl") ||
              Head.substr(I).starts_with("__stdcall") ||
              Head.substr(I).starts_with("__fastcall") ||
              Head.substr(I).starts_with("__thiscall") ||
              Head.substr(I).starts_with("__vectorcall") ||
              Head.substr(I).starts_with("__clrcall")))
      Conv = I;
  }
  if (Conv == StringRef::npos) {
    Proto.ReturnType.clear();
    Proto.ReturnBits = 64;
    return Proto;
  }
  bool IsMember = Head.starts_with("public:") ||
                  Head.starts_with("private:") ||
                  Head.starts_with("protected:") ||
                  Head.starts_with("[thunk]:");
  if (IsMember && !Head.contains("static")) {
    LibraryParam This;
    This.Name = "this";
    This.Type = "void *";
    This.Bits = 64;
    Proto.Params.insert(Proto.Params.begin(), std::move(This));
  }
  StringRef Ret = Head.take_front(Conv).trim();
  for (StringRef Prefix : {"public:", "private:", "protected:", "static ",
                           "virtual ", "[thunk]:", "__ptr64"})
    if (Ret.starts_with(Prefix))
      Ret = Ret.drop_front(Prefix.size()).trim();
  Proto.ReturnVoid = Ret.empty() || Ret == "void";
  Proto.ReturnFloat =
      !Proto.ReturnVoid && !Ret.contains('*') && !Ret.contains('&') &&
      !Ret.contains('(') && (Ret.contains("float") || Ret.contains("double"));
  Proto.ReturnBits = Proto.ReturnVoid ? 0 : slotBits(Ret);
  Proto.ReturnType = Ret.str();
  return Proto;
}


static Error addArchive(LibraryDatabase &DB, StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path);
  if (!Buffer)
    return createFileError(Path, Buffer.getError());
  Expected<std::unique_ptr<Archive>> ArchiveOrErr =
      Archive::create((*Buffer)->getMemBufferRef());
  if (!ArchiveOrErr)
    return ArchiveOrErr.takeError();
  std::unique_ptr<Archive> A = std::move(*ArchiveOrErr);
  Error IterErr = Error::success();
  for (const Archive::Child &Child : A->children(IterErr)) {
    auto MemberName = Child.getName();
    if (!MemberName) {
      consumeError(MemberName.takeError());
      continue;
    }
    auto Member = Child.getAsBinary();
    if (!Member) {
      consumeError(Member.takeError());
      continue;
    }
    auto *Obj = dyn_cast<ObjectFile>(Member->get());
    if (!Obj || Obj->getArch() != Triple::x86_64)
      continue;
    std::vector<std::pair<SymbolRef, uint64_t>> Sized = computeSymbolSizes(*Obj);
    std::map<uint64_t, ArrayRef<uint8_t>> Contents;
    std::map<uint64_t, std::map<uint64_t, uint32_t>> Masks;
    std::map<uint64_t, std::map<uint64_t, std::string>> RelocCallees;
    for (SectionRef Sec : Obj->sections()) {
      auto SectionBytes = Sec.getContents();
      if (!SectionBytes) {
        consumeError(SectionBytes.takeError());
        continue;
      }
      uint64_t SectionIndex = Sec.getIndex();
      Contents.emplace(SectionIndex, ArrayRef<uint8_t>(
                                         reinterpret_cast<const uint8_t *>(
                                             SectionBytes->data()),
                                         SectionBytes->size()));
      auto Relocs = Sec.relocations();
      if (Relocs.empty())
        continue;
      for (const RelocationRef &Reloc : Relocs) {
        SmallString<32> Type;
        Reloc.getTypeName(Type);
        uint32_t Width = 4;
        if (StringRef(Type).contains_insensitive("64"))
          Width = 8;
        Expected<symbol_iterator> Symbol = Reloc.getSymbol();
        if (Symbol && *Symbol != Obj->symbol_end()) {
          Expected<StringRef> Callee = (**Symbol).getName();
          if (Callee)
            RelocCallees[SectionIndex][Reloc.getOffset()] =
                shortName(demangle(Callee->str()));
          else
            consumeError(Callee.takeError());
        } else if (!Symbol) {
          consumeError(Symbol.takeError());
        }
        Masks[SectionIndex][Reloc.getOffset()] = Width;
      }
    }

    for (const auto &[Sym, Size] : Sized) {
      auto Type = Sym.getType();
      if (!Type) {
        consumeError(Type.takeError());
        continue;
      }
      if (*Type != SymbolRef::ST_Function || Size == 0 || Size > UINT32_MAX)
        continue;
      auto SectionOrErr = Sym.getSection();
      if (!SectionOrErr) {
        consumeError(SectionOrErr.takeError());
        continue;
      }
      if (*SectionOrErr == Obj->section_end())
        continue;
      uint64_t SectionIndex = (**SectionOrErr).getIndex();
      auto ContentsIt = Contents.find(SectionIndex);
      if (ContentsIt == Contents.end())
        continue;
      auto Address = Sym.getAddress();
      if (!Address) {
        consumeError(Address.takeError());
        continue;
      }
      uint64_t SectionStart = (**SectionOrErr).getAddress();
      if (*Address < SectionStart)
        continue;
      uint64_t Offset = *Address - SectionStart;
      ArrayRef<uint8_t> SectionBytes = ContentsIt->second;
      if (Offset > SectionBytes.size() || Size > SectionBytes.size() - Offset)
        continue;
      auto Name = Sym.getName();
      if (!Name) {
        consumeError(Name.takeError());
        continue;
      }

      LibraryFunction F;
      std::string Demangled = demangle(Name->str());
      F.Name = shortName(Demangled);
      if (F.Name.empty())
        F.Name = Name->str();
      // MSVC funclets (`?dtor$N`, `?catch$N`, `?fin$N`) of a Rust symbol
      // wrap the v0 name as `?dtor$16@?0?_RNv…@4HA`: recover the parent so
      // the record names the real function, not raw mangled text.
      if (StringRef(*Name).starts_with("?")) {
        size_t Marker = Name->find("@?0?");
        if (Marker != StringRef::npos) {
          StringRef Inner = StringRef(*Name).drop_front(Marker + 4);
          Inner = Inner.take_front(Inner.find('@'));
          std::string Parent = shortName(demangle(Inner.str()));
          if (!Parent.empty() && Parent != Inner)
            F.Name = Parent + "::" +
                     Name->substr(1, Marker - 1).str();
        }
      }
      F.MangledName = Name->str();
      F.Archive = Path.str();
      F.Member = MemberName->str();
      F.Size = Size;
      F.Bytes.assign(SectionBytes.begin() + Offset,
                     SectionBytes.begin() + Offset + Size);
      auto MaskIt = Masks.find(SectionIndex);
      if (MaskIt != Masks.end()) {
        for (const auto &[RelocOffset, Width] : MaskIt->second) {
          if (RelocOffset < Offset || RelocOffset >= Offset + Size)
            continue;
          uint64_t Relative = RelocOffset - Offset;
          for (uint32_t I = 0; I < Width && Relative + I < Size; ++I) {
            F.MaskedRelocations.push_back(static_cast<uint32_t>(Relative + I));
            F.Bytes[Relative + I] = 0;
          }
        }
        auto CalleesIt = RelocCallees.find(SectionIndex);
        if (CalleesIt != RelocCallees.end())
          for (const auto &[RelocOffset, Callee] : CalleesIt->second)
            if (RelocOffset >= Offset && RelocOffset < Offset + Size)
              F.Callees.push_back(Callee);
      }
      llvm::sort(F.MaskedRelocations);
      F.MaskedRelocations.erase(
          std::unique(F.MaskedRelocations.begin(), F.MaskedRelocations.end()),
          F.MaskedRelocations.end());
      llvm::sort(F.Callees);
      F.Callees.erase(std::unique(F.Callees.begin(), F.Callees.end()),
                      F.Callees.end());
      // No header prototype? A mangled name still encodes the signature.
      if (auto Proto = prototypeFromDemangled(Demangled))
        F.Prototype = std::move(*Proto);
      DB.Functions.push_back(std::move(F));
    }
  }
  if (Error E = std::move(IterErr))
    return E;
  return Error::success();
}

static int buildDB() {
  for (const std::string &Header : CHeaders)
    if (Error E = parseHeader(Header, false)) {
      errs() << toString(std::move(E)) << '\n';
      return 1;
    }
  for (const std::string &Header : CxxHeaders)
    if (Error E = parseHeader(Header, true)) {
      errs() << toString(std::move(E)) << '\n';
      return 1;
    }
  LibraryDatabase DB;
  for (const std::string &Path : Inputs)
    if (Error E = addArchive(DB, Path)) {
      errs() << toString(std::move(E)) << '\n';
      return 1;
    }
  for (LibraryFunction &F : DB.Functions) {
    auto It = Prototypes.find(F.MangledName);
    if (It == Prototypes.end())
      It = Prototypes.find(F.Name);
    if (It != Prototypes.end())
      F.Prototype = It->second;
  }
  if (Error E = DB.save(Output)) {
    errs() << toString(std::move(E)) << '\n';
    return 1;
  }
  uint64_t WithProto = 0;
  for (const LibraryFunction &F : DB.Functions)
    WithProto += F.Prototype.has_value();
  outs() << "database functions: " << DB.Functions.size() << " (" << WithProto
         << " with prototypes)\n";
  return 0;
}

static int matchBinary() {
  auto PEOrErr = PeFile::load(TargetBinary);
  if (!PEOrErr) {
    errs() << toString(PEOrErr.takeError()) << '\n';
    return 1;
  }
  auto DBOrErr = LibraryDatabase::load(Output);
  if (!DBOrErr) {
    errs() << toString(DBOrErr.takeError()) << '\n';
    return 1;
  }
  std::unique_ptr<llvm::pdb::IPDBSession> PDBSession;
  if (!PdbPath.empty()) {
    if (Error E = llvm::pdb::NativeSession::createFromPdbPath(PdbPath,
                                                              PDBSession)) {
      errs() << toString(std::move(E)) << '\n';
      return 1;
    }
    PDBSession->setLoadAddress(PEOrErr->Img->Base.Value);
  }
  auto Hooks = createX86TargetHooks();
  if (!Hooks) {
    errs() << "failed to initialize X86 hooks\n";
    return 1;
  }
  Program P = analyze(*PEOrErr, *Hooks);
  auto DisOrErr = Disasm::create(*P.Img);
  if (!DisOrErr) {
    errs() << toString(DisOrErr.takeError()) << '\n';
    return 1;
  }
  Signatures Sigs = analyzeSignatures(P, *DisOrErr);
  std::vector<LibraryMatch> Matches =
      matchLibraryFunctions(P, *DBOrErr);
  applyLibraryMatches(P, Sigs, Matches, *DBOrErr);

  outs() << "function_va,name,archive,member,param_count,pdb_name,correct\n";
  std::set<Va> Printed;
  uint64_t PDBCompared = 0, PDBCorrect = 0;
  for (const LibraryMatch &Match : Matches) {
    if (!Printed.insert(Match.Start).second)
      continue;
    auto Name = P.Names->find(Match.Start);
    if (Name == P.Names->end() ||
        StringRef(Name->second).starts_with("sub_"))
      continue;
    auto Candidate = std::find_if(
        Matches.begin(), Matches.end(), [&](const LibraryMatch &M) {
          if (M.Start != Match.Start)
            return false;
          const std::string &DBName = DBOrErr->Functions[M.FunctionIndex].Name;
          return DBName == Name->second ||
                 Name->second == DBName + "__" + utohexstr(M.Start.Value, true);
        });
    if (Candidate == Matches.end())
      continue;
    const LibraryFunction &F = DBOrErr->Functions[Candidate->FunctionIndex];
    const Signature *Sig = Sigs.of(Match.Start);
    std::string PDBName;
    bool Correct = false;
    if (PDBSession) {
      auto RVA = PEOrErr->Img->rvaOf(Match.Start);
      if (RVA) {
        auto Symbol = PDBSession->findSymbolByRVA(*RVA,
                                                  llvm::pdb::PDB_SymType::Function);
        if (Symbol) {
          PDBName = Symbol->getName();
          // Spelling variants (decorations, closure/hash tokens, template
          // sugar) compare on alphanumeric runs; substring containment
          // covers impl-block spellings like `<T as Trait>::fmt`.
          std::string Got = normKey(F.Name);
          std::string Want = normKey(PDBName);
          Correct = !Want.empty() &&
                    (Got == Want || Got.find(Want) != std::string::npos ||
                     Want.find(Got) != std::string::npos);
          ++PDBCompared;
          PDBCorrect += Correct;
        }
      }
    }
    outs() << format_hex_no_prefix(Match.Start.Value, 0, true) << ','
           << '"' << F.Name << "\",\"" << F.Archive << "\",\""
           << F.Member << "\"," << (Sig ? Sig->Params.size() : 0) << ",\""
           << PDBName << "\"," << Correct << '\n';
  }
  if (PDBSession)
    errs() << "PDB name matches: " << PDBCorrect << '/' << PDBCompared << '\n';
  return 0;
}

int main(int Argc, char **Argv) {
  InitLLVM X(Argc, Argv);
  cl::ParseCommandLineOptions(Argc, Argv, "static library match database\n");
  if (Mode == "build")
    return buildDB();
  if (Mode == "match" && !TargetBinary.empty())
    return matchBinary();
  errs() << "usage: revenant-libdb build -o <db.json> <archive>...\n"
            "       revenant-libdb match -o <db.json> -binary <pe.exe>\n";
  return 2;
}
