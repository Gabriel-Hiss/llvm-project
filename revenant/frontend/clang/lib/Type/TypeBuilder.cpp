//===- TypeBuilder.cpp - DI/IR types -> clang::QualType --------*- C++ -*-===//
//
// Implements revenant::clangx::TypeBuilder (interface in
// include/revenant/Clang/TypeBuilder.h, owned by Swe2ClangCore):
// integer widths with DI signedness, pointers, arrays, function pointers,
// structs/unions from DICompositeType, and the C++ layer (CXXRecordDecl with
// methods recovered from mangled names via LLVM's Demangle library, `this`
// parameters, vtable globals -> virtual methods, vcall slot lookup,
// namespaces, RTTI names).
//
// Reference rules ported from revenant-emit-cpp and revenant-demangle in
// C:\Users\User\RustroverProjects\Revenant\.worktrees\h-merge\crates\:
//  - emit-cpp: `this` parameter of member functions, `this->field` naming by
//    offset, `Class::method` qualified names, virtual calls through
//    `*(*recv + k)` resolving via the class vtable map to `recv->method`,
//    `nullptr` in pointer context, class shells with bases first, unknown
//    types as uintptr_t.
//  - demangle: MSVC `?x@@` via the MSVC demangler, Itanium `_Z`, short names
//    dropping parameter lists and hash suffixes.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/TypeBuilder.h"
#include "RealTypeBuilder.h"
#include "revenant/Clang/FrontendContext.h"

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Type.h"
#include "clang/Basic/SourceLocation.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Casting.h"
#include <cctype>
#include <cstdlib>
#include <memory>
#include <string>

using namespace llvm;
using llvm::dwarf::Tag;

namespace revenant::clangx {
namespace {

// ---------------------------------------------------------------------------
// Demangle helpers (port of revenant-demangle, backed by LLVM's Demangle)
// ---------------------------------------------------------------------------

enum class Scheme { Msvc, Itanium, None };

struct Demangled {
  std::string Full;
  std::string Short;
  Scheme Which = Scheme::None;
};

// Drop the top-level parameter list: the first `(` outside `<>`.
// Port of demangle/src/lib.rs `short_from_full`.
std::string shortFromFull(StringRef Full) {
  int Depth = 0;
  for (size_t I = 0; I < Full.size(); ++I) {
    char C = Full[I];
    if (C == '<')
      ++Depth;
    else if (C == '>')
      --Depth;
    else if (C == '(' && Depth == 0)
      return std::string(Full.substr(0, I));
  }
  return std::string(Full);
}

Demangled demangleMsvc(StringRef Sym) {
  Demangled D;
  D.Which = Scheme::Msvc;
  int Status = 0;
  char *Full = microsoftDemangle(Sym, nullptr, &Status);
  if (!Full || Status != demangle_success) {
    free(Full);
    D.Full = std::string(Sym);
    D.Short = std::string(Sym);
    D.Which = Scheme::None;
    return D;
  }
  D.Full = Full;
  D.Short = shortFromFull(D.Full);
  free(Full);
  return D;
}

Demangled demangleItanium(StringRef Sym) {
  Demangled D;
  char *Full = itaniumDemangle(Sym, /*ParseParams=*/true);
  if (!Full) {
    D.Full = std::string(Sym);
    D.Short = std::string(Sym);
    D.Which = Scheme::None;
    return D;
  }
  D.Which = Scheme::Itanium;
  D.Full = Full;
  D.Short = shortFromFull(D.Full);
  free(Full);
  return D;
}

// Port of demangle(): PE import names may carry a leading 0x01; `?`
// selects MSVC, `_Z` selects Itanium.
Demangled demangleSymbol(StringRef Sym) {
  if (!Sym.empty() && Sym.front() == '\x01')
    Sym = Sym.drop_front();
  if (Sym.starts_with('?'))
    return demangleMsvc(Sym);
  // Strip one cdecl '_' decoration only when the rest looks Itanium.
  StringRef Probe = Sym;
  if (Probe.starts_with('_') && Probe.drop_front().starts_with("_Z"))
    Probe = Probe.drop_front();
  if (Probe.starts_with("_Z") || Probe.starts_with("__Z"))
    return demangleItanium(Probe);
  Demangled D;
  D.Full = std::string(Sym);
  D.Short = std::string(Sym);
  return D;
}

/// Split "A::B::leaf" into {"A::B", "leaf"}. Returns false when unqualified.
bool splitQualified(StringRef Name, std::string &Scope, std::string &Leaf) {
  size_t Pos = Name.rfind("::");
  if (Pos == StringRef::npos)
    return false;
  Scope = std::string(Name.substr(0, Pos));
  Leaf = std::string(Name.substr(Pos + 2));
  return true;
}

// Port of emit-cpp `sanitize`: keep `::` and `~`, map runs of other
// punctuation to a single `_`, trim trailing `_`, prefix digit-led names.
std::string sanitizeName(StringRef Name) {
  bool Flatten = Name.contains('`') || Name.contains('$');
  std::string Out;
  Out.reserve(Name.size());
  bool PrevUs = false;
  for (size_t I = 0; I < Name.size(); ++I) {
    char C = Name[I];
    bool Alnum = (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') ||
                 (C >= '0' && C <= '9') || C == '_' ||
                 (static_cast<unsigned char>(C) >= 0x80);
    if (Alnum) {
      Out.push_back(C);
      PrevUs = false;
    } else if (!Flatten && C == ':' && I + 1 < Name.size() &&
               Name[I + 1] == ':') {
      Out += "::";
      ++I;
      PrevUs = false;
    } else if (C == '~') {
      Out.push_back('~');
      PrevUs = false;
    } else if (!PrevUs) {
      Out.push_back('_');
      PrevUs = true;
    }
  }
  while (!Out.empty() && Out.back() == '_')
    Out.pop_back();
  if (!Name.empty()) {
    char First = Name.front();
    bool FirstOk = (First >= 'A' && First <= 'Z') ||
                   (First >= 'a' && First <= 'z') || First == '_' ||
                   First == '~' || (static_cast<unsigned char>(First) >= 0x80);
    if (!FirstOk) {
      size_t P = Out.find_first_not_of('_');
      Out = (P == std::string::npos) ? "" : Out.substr(P);
    }
  }
  if (Out.empty())
    return "_";
  if (Out.front() >= '0' && Out.front() <= '9')
    return "_" + Out;
  return Out;
}

// Port of emit-cpp `member_method_name`: operator spellings stay real.
std::string memberMethodName(StringRef Name) {
  if (Name.starts_with("operator")) {
    StringRef Rest = Name.drop_front(sizeof("operator") - 1).trim();
    if (Rest.empty())
      return "operator";
    if (std::isalnum(static_cast<unsigned char>(Rest.front())))
      return "operator " + Rest.str();
    return "operator" + Rest.str();
  }
  return sanitizeName(Name);
}

} // namespace

// ---------------------------------------------------------------------------
// RealTypeBuilder
// ---------------------------------------------------------------------------

class RealTypeBuilderImpl final : public RealTypeBuilder {
public:
  explicit RealTypeBuilderImpl(FrontendContext &Ctx) : Ctx(Ctx) {}

  clang::QualType typeOf(Type *Ty, const DIType *DI) override {
    if (DI) {
      clang::QualType Q = typeOfDI(DI);
      if (!Q.isNull()) {
        bool KnownIRCategory = Ty &&
                               (Ty->isIntegerTy() || Ty->isFloatingPointTy() ||
                                Ty->isPointerTy() || Ty->isStructTy() ||
                                Ty->isArrayTy() || Ty->isVoidTy() ||
                                Ty->isVectorTy());
        bool Compatible =
            !KnownIRCategory ||
            (Ty->isIntegerTy() && Q->isIntegerType()) ||
            (Ty->isFloatingPointTy() && Q->isFloatingType()) ||
            (Ty->isPointerTy() && Q->isPointerType()) ||
            (Ty->isStructTy() && Q->isRecordType()) ||
            (Ty->isArrayTy() && Q->isArrayType()) ||
            (Ty->isVoidTy() && Q->isVoidType()) ||
            (Ty->isVectorTy() && Q->isVectorType());
        if (Compatible)
          return Q;
      }
    }
    return typeOfIR(Ty);
  }

  clang::QualType typeOfDI(const DIType *DI) override {
    if (!DI)
      return uintptrT();
    if (clang::QualType Q = mapDI(DI, 0); !Q.isNull())
      return Q;
    return uintptrT();
  }

  clang::QualType functionType(Function &F) override {
    // Prefer DISubprogram element types over the IR signature (interface
    // contract: recovered signature lives in DI). Arity still comes from
    // the IR: callers create one ParmVarDecl per Argument, so the proto
    // must have F.arg_size() params. DI lists [ret, p1..pN] and may be
    // shorter (partial recovery) or longer (elements beyond params).
    unsigned IRArity = F.arg_size();
    if (DISubprogram *SP = F.getSubprogram()) {
      if (const auto *Sig = dyn_cast_or_null<DISubroutineType>(SP->getType())) {
        clang::QualType Ret = typeOfIR(F.getReturnType());
        SmallVector<clang::QualType, 8> Params;
        bool First = true;
        for (DIType *T : Sig->getTypeArray()) {
          if (First) {
            First = false;
            if (T)
              Ret = mapDIOrFallback(T);
            continue;
          }
          if (Params.size() >= IRArity)
            break;
          Params.push_back(T ? mapDIOrFallback(T) : uintptrT());
        }
        // Partial or missing DI params: fill from the IR signature.
        unsigned P = 0;
        for (Type *PT : F.getFunctionType()->params()) {
          if (P++ < Params.size())
            continue;
          Params.push_back(typeOfIR(PT));
        }
        return buildFunctionType(Ret, Params, F.getFunctionType()->isVarArg());
      }
    }
    clang::QualType Ret = typeOfIR(F.getReturnType());
    SmallVector<clang::QualType, 8> Params;
    for (Type *P : F.getFunctionType()->params())
      Params.push_back(typeOfIR(P));
    return buildFunctionType(Ret, Params, F.getFunctionType()->isVarArg());
  }

  clang::QualType globalType(GlobalVariable &GV) override {
    // A global is declared with the pointee type, not a pointer to it.
    SmallVector<DIGlobalVariableExpression *, 1> GVs;
    GV.getDebugInfo(GVs);
    if (!GVs.empty()) {
      if (const DIGlobalVariable *V = GVs[0]->getVariable()) {
        if (const DIType *T = V->getType()) {
          if (clang::QualType Q = mapDIOrFallback(T); !Q.isNull())
            return Q;
        }
      }
    }
    return typeOfIR(GV.getValueType());
  }

  clang::RecordDecl *recordFor(const DICompositeType *DI) override {
    if (!DI || !isRecordTag(DI->getTag()))
      return nullptr;
    return materializeRecord(DI);
  }

  const DISubprogram *virtualMethod(const DICompositeType *RecordDI,
                                    unsigned Slot) override {
    if (!RecordDI)
      return nullptr;
    // Primary: vtable-holder element scan — the contract records vftable
    // layout as subprogram entries; match the byte offset.
    if (const DISubprogram *SP = virtualFromElements(RecordDI, Slot))
      return SP;
    // Fallback: mangled-name recovery from vtable globals.
    return virtualFromVTableGlobal(RecordDI, Slot);
  }

  clang::CXXMethodDecl *methodFor(const DISubprogram *SP) override {
    if (!SP)
      return nullptr;
    auto It = MethodDecls.find(SP);
    return It == MethodDecls.end() ? nullptr : It->second;
  }

  // -- C++ layer entry points used by Core/ExprBuilder ---------------------

  /// Register a vtable global: `@Class_vftable = constant [N x ptr] [...]`.
  /// Slots map to function values whose DISubprogram (or demangled name)
  /// recovers the virtual method. Mirrors emit-cpp's class vtable map.
  void addVTable(StringRef ClassName, GlobalVariable *GV) override {
    VTables[ClassName.str()] = GV;
    ensureNamespaceChain(ClassName);
    // RTTI name: `.?AVClass@@` style globals reference the class; make sure
    // the record shell exists so RTTI users resolve.
    (void)rttiNameFor(ClassName);
  }

  /// Recover method decls from a mangled function name (MSVC or Itanium via
  /// LLVM Demangle) and attach the method to its record. Returns the
  /// materialized method or nullptr.
  clang::CXXMethodDecl *recoverMethod(Function &F) override {
    DISubprogram *SP = F.getSubprogram();
    StringRef Linkage =
        SP ? SP->getLinkageName() : StringRef(F.getName()).trim();
    if (Linkage.empty())
      return nullptr;
    Demangled D = demangleSymbol(Linkage);
    if (D.Which == Scheme::None)
      return nullptr;
    std::string Scope, Leaf;
    if (!splitQualified(StringRef(D.Short).trim(), Scope, Leaf))
      return nullptr;
    clang::CXXRecordDecl *RD = ensureRecordShell(Scope);
    if (!RD)
      return nullptr;
    return attachMethod(RD, Leaf, F, SP);
  }

  /// The `this` parameter type for a method of record `RD`: `RD *`, const-
  /// qualified when the method is const. Port of emit-cpp's implicit `this`
  /// (member fns carry `this` at index 0).
  clang::QualType thisType(clang::CXXRecordDecl *RD, bool IsConst) override {
    clang::QualType Rec = Ctx.ast().getCanonicalTagType(RD);
    if (IsConst)
      Rec.addConst();
    return Ctx.ast().getPointerType(Rec);
  }

private:
  FrontendContext &Ctx;
  DenseMap<const DICompositeType *, clang::RecordDecl *> Records;
  DenseMap<const DISubprogram *, clang::CXXMethodDecl *> MethodDecls;
  StringMap<GlobalVariable *> VTables;
  StringMap<clang::NamespaceDecl *> Namespaces;
  StringMap<clang::CXXRecordDecl *> Shells;

  // -- primitive type mapping ----------------------------------------------

  clang::QualType uintptrT() { return Ctx.ast().UnsignedLongLongTy; }

  clang::QualType voidTy() { return Ctx.ast().VoidTy; }

  clang::QualType voidPtr() { return Ctx.ast().getPointerType(voidTy()); }

  /// Integer of `Bits` width; signedness from DI, defaulting to unsigned
  /// (matches NullTypeBuilder and emit-cpp's `u` prefix convention).
  clang::QualType intOf(unsigned Bits, bool Signed) {
    clang::ASTContext &A = Ctx.ast();
    switch (Bits) {
    case 1:
      return A.BoolTy;
    case 8:
      return Signed ? A.SignedCharTy : A.UnsignedCharTy;
    case 16:
      return Signed ? A.ShortTy : A.UnsignedShortTy;
    case 32:
      return Signed ? A.IntTy : A.UnsignedIntTy;
    case 64:
      return Signed ? A.LongLongTy : A.UnsignedLongLongTy;
    case 128: {
      // MSVC does not support __int128. Use target-conditional:
      // gcc/clang targets get unsigned __int128; MSVC gets struct rv_u128
      // with per-TU helpers (see TranslationUnit.cpp).
      clang::ASTContext &A = Ctx.ast();
      llvm::StringRef Triple = Ctx.module().getTargetTriple().str();
      bool IsMSVC = Triple.contains("msvc");
      if (!IsMSVC) {
        return Signed ? A.Int128Ty : A.UnsignedInt128Ty;
      }
      // MSVC: struct rv_u128 { unsigned __int64 lo, hi; }
      clang::IdentifierInfo &II = A.Idents.get("rv_u128");
      clang::TagDecl *TD = nullptr;
      for (clang::NamedDecl *D : A.getTranslationUnitDecl()->lookup(&II))
        if (auto *T = llvm::dyn_cast<clang::TagDecl>(D))
          TD = T;
      if (!TD) {
        TD = clang::RecordDecl::Create(A, clang::TagTypeKind::Struct,
                                       A.getTranslationUnitDecl(),
                                       clang::SourceLocation(),
                                       clang::SourceLocation(), &II);
        TD->setImplicit(false);
        A.getTranslationUnitDecl()->addDecl(TD);
        clang::QualType U64Ty = A.UnsignedLongLongTy;
        clang::IdentifierInfo &LoII = A.Idents.get("lo");
        clang::FieldDecl *LoFD = clang::FieldDecl::Create(A, TD,
            clang::SourceLocation(), clang::SourceLocation(), &LoII,
            U64Ty, nullptr, nullptr, false, clang::ICIS_NoInit);
        TD->addDecl(LoFD);
        clang::IdentifierInfo &HiII = A.Idents.get("hi");
        clang::FieldDecl *HiFD = clang::FieldDecl::Create(A, TD,
            clang::SourceLocation(), clang::SourceLocation(), &HiII,
            U64Ty, nullptr, nullptr, false, clang::ICIS_NoInit);
        TD->addDecl(HiFD);
        llvm::cast<clang::RecordDecl>(TD)->completeDefinition();
      }
      return A.getCanonicalTagType(TD);
    }
    default:
      return uintptrT();
    }
  }

  clang::QualType floatOf(unsigned Bits) {
    clang::ASTContext &A = Ctx.ast();
    switch (Bits) {
    case 16:
      return A.HalfTy;
    case 32:
      return A.FloatTy;
    case 64:
      return A.DoubleTy;
    case 80:
      return A.LongDoubleTy; // x87 extended
    case 128:
      return A.Float128Ty;
    default:
      return A.DoubleTy;
    }
  }

  // -- IR type mapping (no DI) ----------------------------------------------

  clang::QualType typeOfIR(Type *Ty) {
    if (!Ty)
      return uintptrT();
    clang::ASTContext &A = Ctx.ast();
    if (Ty->isVoidTy())
      return A.VoidTy;
    if (Ty->isIntegerTy())
      return intOf(Ty->getIntegerBitWidth(), /*Signed=*/false);
    if (Ty->isHalfTy() || Ty->isFloatTy() || Ty->isDoubleTy() ||
        Ty->isX86_FP80Ty() || Ty->isFP128Ty())
      return floatOf(Ty->getPrimitiveSizeInBits());
    if (Ty->isPointerTy())
      return voidPtr();
    if (auto *AT = dyn_cast<ArrayType>(Ty)) {
      clang::QualType El = typeOfIR(AT->getElementType());
      return A.getConstantArrayType(El, llvm::APInt(64, AT->getNumElements()),
                                    nullptr, clang::ArraySizeModifier::Normal,
                                    0);
    }
    if (auto *VT = dyn_cast<FixedVectorType>(Ty)) {
      TypeSize Size = Ctx.module().getDataLayout().getTypeSizeInBits(VT);
      if (!Size.isScalable())
        return intOf(static_cast<unsigned>(Size.getFixedValue()),
                     /*Signed=*/false);
    }
    if (auto *ST = dyn_cast<StructType>(Ty)) {
      if (ST->isOpaque())
        return voidPtr();
      // Anonymous IR structs become byte arrays sized to the layout.
      uint64_t Sz = ST->getNumElements() * 8;
      clang::QualType El = A.UnsignedCharTy;
      return A.getConstantArrayType(El, llvm::APInt(64, Sz ? Sz : 1), nullptr,
                                    clang::ArraySizeModifier::Normal, 0);
    }
    if (auto *FT = dyn_cast<FunctionType>(Ty)) {
      clang::QualType Ret = typeOfIR(FT->getReturnType());
      SmallVector<clang::QualType, 8> Params;
      for (Type *P : FT->params())
        Params.push_back(typeOfIR(P));
      return buildFunctionType(Ret, Params, FT->isVarArg());
    }
    return uintptrT();
  }

  clang::QualType buildFunctionType(clang::QualType Ret,
                                    ArrayRef<clang::QualType> Params,
                                    bool Variadic) {
    // C and C++ cannot return arrays. Lifted aggregate-array returns decay
    // to pointers in the emitted source signature.
    if (const auto *AT = Ret->getAsArrayTypeUnsafe())
      Ret = Ctx.ast().getPointerType(AT->getElementType());
    // C has no `f(...)` without a named parameter before C23; the opaque
    // revenant.asm.* declarations are unprototyped there instead.
    if (Variadic && Params.empty() && !Ctx.isCPlusPlus())
      return Ctx.ast().getFunctionNoProtoType(Ret);
    clang::FunctionProtoType::ExtProtoInfo EPI;
    EPI.Variadic = Variadic;
    return Ctx.ast().getFunctionType(Ret, Params, EPI);
  }

  // -- DI type mapping ---------------------------------------------------------

  static bool isRecordTag(dwarf::Tag T) {
    return T == dwarf::DW_TAG_structure_type ||
           T == dwarf::DW_TAG_class_type || T == dwarf::DW_TAG_union_type;
  }

  /// Signedness of a DIBasicType: DWARF `DW_ATE_signed`/`signed_char` are
  /// signed; everything else unsigned (matches emit-cpp output, which uses
  /// the `u` prefix unless use says signed).
  static bool basicIsSigned(const DIBasicType *B) {
    switch (B->getEncoding()) {
    case dwarf::DW_ATE_signed:
    case dwarf::DW_ATE_signed_char:
      return true;
    default:
      return false;
    }
  }

  clang::QualType mapDIOrFallback(const DIType *DI) {
    if (clang::QualType Q = mapDI(DI, 0); !Q.isNull())
      return Q;
    return uintptrT();
  }

  clang::QualType mapDI(const DIType *DI, unsigned Depth) {
    if (!DI || Depth > 32)
      return clang::QualType();
    // Peel typedefs / qualifiers.
    if (const auto *DD = dyn_cast<DIDerivedType>(DI)) {
      switch (DD->getTag()) {
      case dwarf::DW_TAG_typedef:
      case dwarf::DW_TAG_const_type:
      case dwarf::DW_TAG_volatile_type:
      case dwarf::DW_TAG_restrict_type:
      case dwarf::DW_TAG_atomic_type: {
        clang::QualType Inner = mapDI(DD->getBaseType(), Depth + 1);
        if (Inner.isNull())
          return clang::QualType();
        if (DD->getTag() == dwarf::DW_TAG_const_type)
          Inner.addConst();
        if (DD->getTag() == dwarf::DW_TAG_volatile_type)
          Inner.addVolatile();
        return Inner;
      }
      case dwarf::DW_TAG_pointer_type: {
        const DIType *Base = DD->getBaseType();
        if (!Base)
          return voidPtr();
        // vtable-pointer shape (ptr -> ptr -> subroutine): the middle
        // pointer carries no element info; emit void** rather than a
        // triple-pointer-to-function the printer renders badly.
        if (const auto *Inner = dyn_cast<DIDerivedType>(Base)) {
          if (Inner->getTag() == dwarf::DW_TAG_pointer_type) {
            if (const DIType *InnerBase = Inner->getBaseType()) {
              if (isa<DISubroutineType>(InnerBase) ||
                  (isa<DIDerivedType>(InnerBase) &&
                   cast<DIDerivedType>(InnerBase)->getTag() ==
                       dwarf::DW_TAG_subroutine_type))
                return Ctx.ast().getPointerType(voidPtr());
            }
          }
        }
        clang::QualType Inner = mapDI(Base, Depth + 1);
        if (Inner.isNull())
          return voidPtr();
        return Ctx.ast().getPointerType(Inner);
      }
      case dwarf::DW_TAG_reference_type:
      case dwarf::DW_TAG_rvalue_reference_type: {
        const DIType *Base = DD->getBaseType();
        clang::QualType Inner =
            Base ? mapDI(Base, Depth + 1) : clang::QualType();
        if (Inner.isNull())
          Inner = uintptrT();
        return Ctx.ast().getLValueReferenceType(Inner);
      }
      case dwarf::DW_TAG_ptr_to_member_type:
        return uintptrT();
      case dwarf::DW_TAG_subroutine_type: {
        // Function pointer type inside DI: element 0 is the return.
        // Note: plain subroutine DI has no element list (only
        // DISubroutineType carries a type array); fall through to the
        // pointer-to-uintptr fallback.
        return uintptrT();
      }
      case dwarf::DW_TAG_inheritance:
      case dwarf::DW_TAG_member:
      case dwarf::DW_TAG_variable:
        if (const DIType *Base = DD->getBaseType())
          return mapDI(Base, Depth + 1);
        return clang::QualType();
      default:
        if (const DIType *Base = DD->getBaseType())
          return mapDI(Base, Depth + 1);
        return clang::QualType();
      }
    }
    if (const auto *B = dyn_cast<DIBasicType>(DI)) {
      uint64_t Bits = B->getSizeInBits();
      switch (B->getEncoding()) {
      case dwarf::DW_ATE_address:
        return Ctx.ast().getPointerType(voidTy());
      case dwarf::DW_ATE_boolean:
        return Ctx.ast().BoolTy;
      case dwarf::DW_ATE_float:
        return floatOf(static_cast<unsigned>(Bits ? Bits : 32));
      case dwarf::DW_ATE_UTF:
      case dwarf::DW_ATE_UCS:
      case dwarf::DW_ATE_ASCII:
        return Ctx.ast().UnsignedCharTy;
      default:
        break;
      }
      if (B->getName() == "void" || Bits == 0)
        return voidTy();
      return intOf(static_cast<unsigned>(Bits ? Bits : 64), basicIsSigned(B));
    }
    if (const auto *C = dyn_cast<DICompositeType>(DI)) {
      switch (C->getTag()) {
      case dwarf::DW_TAG_array_type: {
        // Element type is the base; bound from subrange elements.
        const DIType *Base = C->getBaseType();
        clang::QualType El =
            Base ? mapDIOrFallback(Base) : Ctx.ast().UnsignedCharTy;
        uint64_t Count = arrayCount(C);
        return Ctx.ast().getConstantArrayType(
            El, llvm::APInt(64, Count ? Count : 1), nullptr,
            clang::ArraySizeModifier::Normal, 0);
      }
      case dwarf::DW_TAG_enumeration_type: {
        uint64_t Bits = C->getSizeInBits();
        return intOf(static_cast<unsigned>(Bits ? Bits : 32),
                     /*Signed=*/true);
      }
      default:
        break;
      }
      if (isRecordTag(C->getTag())) {
        if (clang::RecordDecl *RD = materializeRecord(C))
          return Ctx.ast().getCanonicalTagType(RD);
        return clang::QualType();
      }
      return clang::QualType();
    }
    if (const auto *SS = dyn_cast<DISubroutineType>(DI)) {
      clang::QualType Ret = voidTy();
      SmallVector<clang::QualType, 8> Params;
      bool First = true;
      for (DIType *T : SS->getTypeArray()) {
        if (First) {
          First = false;
          if (T)
            Ret = mapDIOrFallback(T);
          continue;
        }
        Params.push_back(T ? mapDIOrFallback(T) : uintptrT());
      }
      return Ctx.ast().getPointerType(
          buildFunctionType(Ret, Params, /*Variadic=*/false));
    }
    return clang::QualType();
  }

  static uint64_t arrayCount(const DICompositeType *C) {
    for (DINode *El : C->getElements()) {
      if (const auto *Sub = dyn_cast<DISubrange>(El)) {
        if (Sub->getCount().is<ConstantInt *>())
          return Sub->getCount().get<ConstantInt *>()->getZExtValue();
      }
    }
    return 0;
  }

  // -- records -------------------------------------------------------------------

  /// Sanitize + split scope for nesting: "ns::Outer::Inner" -> namespace
  /// chain + record name; single names land in the TU.
  clang::DeclContext *scopeFor(StringRef Qualified) {
    std::string Scope, Leaf;
    if (!splitQualified(Qualified, Scope, Leaf))
      return Ctx.ast().getTranslationUnitDecl();
    // Nested class scopes ("Outer::Inner"): reuse the outer record when it
    // exists, else fall back to namespace nesting.
    if (clang::CXXRecordDecl *Outer = findShell(Scope))
      return Outer;
    clang::DeclContext *DC = Ctx.ast().getTranslationUnitDecl();
    size_t Start = 0;
    while (true) {
      size_t Pos = Scope.find("::", Start);
      StringRef Part = (Pos == std::string::npos)
                           ? StringRef(Scope).substr(Start)
                           : StringRef(Scope).substr(Start, Pos - Start);
      DC = ensureNamespace(DC, Part);
      if (Pos == std::string::npos)
        break;
      Start = Pos + 2;
    }
    return DC;
  }

  clang::NamespaceDecl *ensureNamespace(clang::DeclContext *DC,
                                        StringRef Name) {
    std::string Key =
        std::string(DC->getDeclKindName()) + "::" + std::string(Name);
    auto It = Namespaces.find(Key);
    if (It != Namespaces.end())
      return It->second;
    clang::IdentifierInfo *II = Ctx.ident(Name);
    auto *NS = clang::NamespaceDecl::Create(
        Ctx.ast(), DC, /*Inline=*/false, clang::SourceLocation(),
        clang::SourceLocation(), II, /*PrevDecl=*/nullptr, /*Nested=*/false);
    DC->addDecl(NS);
    Namespaces[Key] = NS;
    return NS;
  }

  void ensureNamespaceChain(StringRef Qualified) {
    std::string Scope, Leaf;
    if (splitQualified(Qualified, Scope, Leaf))
      (void)scopeFor(Qualified);
  }

  clang::CXXRecordDecl *findShell(StringRef Name) {
    auto It = Shells.find(std::string(Name));
    return It == Shells.end() ? nullptr : It->second;
  }

  /// Create (or return) an empty struct/class shell in the right scope.
  /// Bases are attached later by materializeRecord when DI is available.
  clang::CXXRecordDecl *ensureRecordShell(StringRef Qualified) {
    std::string Clean = sanitizeName(Qualified);
    if (clang::CXXRecordDecl *RD = findShell(Clean))
      return RD;
    std::string Scope, Leaf;
    clang::DeclContext *DC = Ctx.ast().getTranslationUnitDecl();
    StringRef RecName = Clean;
    if (splitQualified(Clean, Scope, Leaf)) {
      DC = scopeFor(Clean);
      RecName = Leaf;
    }
    clang::IdentifierInfo *II = Ctx.ident(std::string(RecName));
    auto *RD = clang::CXXRecordDecl::Create(
        Ctx.ast(), clang::TagTypeKind::Struct, DC, clang::SourceLocation(),
        clang::SourceLocation(), II, /*PrevDecl=*/nullptr);
    RD->setImplicit(false);
    DC->addDecl(RD);
    RD->startDefinition();
    Shells[Clean] = RD;
    return RD;
  }

  /// RTTI display name for a class: MSVC `.?AVClass@@` type descriptor
  /// spelling (detect.cpp flags `.?AV`/`.?AU` strings as RTTI). Used to
  /// recognize RTTI globals that reference the class.
  std::string rttiNameFor(StringRef ClassName) {
    std::string Leaf = std::string(ClassName);
    size_t Pos = Leaf.rfind("::");
    if (Pos != std::string::npos)
      Leaf = Leaf.substr(Pos + 2);
    return ".?AV" + Leaf + "@@";
  }

  /// Full record construction from DICompositeType: fields at their byte
  /// offsets, base classes first (port of `emit_class_chain`), methods from
  /// DISubprogram elements, virtuals flagged. `this` stays implicit in
  /// Clang (unlike emit-cpp's explicit first param): methodFor/virtualMethod
  /// answer the queries ExprBuilder needs instead.
  clang::RecordDecl *materializeRecord(const DICompositeType *DI) {
    auto It = Records.find(DI);
    if (It != Records.end())
      return It->second;
    StringRef Name = DI->getName();
    if (Name.empty())
      Name = DI->getIdentifier();
    if (Name.empty())
      return nullptr;
    clang::CXXRecordDecl *RD = ensureRecordShell(std::string(Name));
    Records[DI] = RD;

    // Bases first (emit_class_chain order): DW_TAG_inheritance entries.
    SmallVector<clang::CXXBaseSpecifier *, 4> Bases;
    for (DINode *El : DI->getElements()) {
      const auto *DD = dyn_cast<DIDerivedType>(El);
      if (!DD || DD->getTag() != dwarf::DW_TAG_inheritance)
        continue;
      const DIType *Base = DD->getBaseType();
      const auto *BC = dyn_cast_or_null<DICompositeType>(Base);
      if (!BC)
        continue;
      clang::RecordDecl *BRD = materializeRecord(BC);
      if (!BRD)
        continue;
      clang::QualType BT = Ctx.ast().getCanonicalTagType(BRD);
      auto *Spec = new (Ctx.ast()) clang::CXXBaseSpecifier(
          clang::SourceRange(), /*Virtual=*/false, /*BaseOfClass=*/true,
          clang::AS_public, Ctx.ast().getTrivialTypeSourceInfo(BT),
          clang::SourceLocation());
      Bases.push_back(Spec);
    }
    if (!Bases.empty())
      RD->setBases(Bases.data(), Bases.size());

    // Members: fields, methods (DISubprogram), static members.
    for (DINode *El : DI->getElements()) {
      if (const auto *DD = dyn_cast<DIDerivedType>(El)) {
        if (DD->getTag() == dwarf::DW_TAG_member)
          addField(RD, DD);
        else if (DD->getTag() == dwarf::DW_TAG_inheritance)
          continue;
        else if (DD->getTag() == dwarf::DW_TAG_variable &&
                 !DD->getName().empty())
          addStaticMember(RD, DD);
      } else if (const auto *SP = dyn_cast<DISubprogram>(El)) {
        attachSubprogram(RD, SP);
      }
    }

    if (!RD->isCompleteDefinition())
      RD->completeDefinition();
    return RD;
  }

  void addField(clang::CXXRecordDecl *RD, const DIDerivedType *DD) {
    StringRef FName = DD->getName();
    if (FName.empty())
      return;
    const DIType *Base = DD->getBaseType();
    clang::QualType FT = Base ? mapDIOrFallback(Base) : uintptrT();
    clang::IdentifierInfo *II = Ctx.ident(std::string(FName));
    auto *FD = clang::FieldDecl::Create(
        Ctx.ast(), RD, clang::SourceLocation(), clang::SourceLocation(), II,
        FT, Ctx.ast().getTrivialTypeSourceInfo(FT), /*BW=*/nullptr,
        /*Mutable=*/false, clang::ICIS_NoInit);
    FD->setAccess(clang::AS_public);
    // Bitfield width when the DI size differs from the type size.
    if (DD->getSizeInBits() && FT->isIntegerType()) {
      uint64_t TyBits =
          Ctx.ast().getTypeSize(FD->getType().getCanonicalType());
      if (DD->getSizeInBits() != TyBits && DD->getSizeInBits() < TyBits) {
        llvm::APInt W(32, DD->getSizeInBits());
        auto *BW = clang::IntegerLiteral::Create(
            Ctx.ast(), W, Ctx.ast().UnsignedIntTy, clang::SourceLocation());
        FD->setBitWidth(BW);
      }
    }
    RD->addDecl(FD);
  }

  void addStaticMember(clang::CXXRecordDecl *RD, const DIDerivedType *DD) {
    const DIType *Base = DD->getBaseType();
    clang::QualType FT = Base ? mapDIOrFallback(Base) : uintptrT();
    clang::IdentifierInfo *II = Ctx.ident(std::string(DD->getName()));
    auto *VD = clang::VarDecl::Create(
        Ctx.ast(), RD, clang::SourceLocation(), clang::SourceLocation(), II,
        FT, Ctx.ast().getTrivialTypeSourceInfo(FT), clang::SC_Static);
    VD->setAccess(clang::AS_public);
    RD->addDecl(VD);
  }

  /// Attach a DISubprogram element as a CXXMethodDecl: virtuals flagged
  /// from SP flags/virtual index (port of emit-cpp's `virtuals` map and
  /// `method_decl` virtual/const rendering).
  clang::CXXMethodDecl *attachSubprogram(clang::CXXRecordDecl *RD,
                                         const DISubprogram *SP) {
    if (MethodDecls.count(SP))
      return MethodDecls[SP];
    StringRef Leaf = SP->getName();
    std::string LeafStorage;
    if (Leaf.empty()) {
      Demangled D = demangleSymbol(SP->getLinkageName());
      std::string Scope, L;
      if (splitQualified(StringRef(D.Short).trim(), Scope, L))
        LeafStorage = L;
      else
        LeafStorage = D.Short;
      Leaf = LeafStorage;
    }
    std::string MethodName = memberMethodName(Leaf);
    return buildMethod(RD, MethodName, SP);
  }

  clang::CXXMethodDecl *attachMethod(clang::CXXRecordDecl *RD,
                                     StringRef Leaf, Function &F,
                                     DISubprogram *SP) {
    std::string MethodName = memberMethodName(Leaf);
    if (MethodName == "__vecDelDtor" ||
        MethodName == "__local_vftable_ctor_closure")
      return nullptr; // compiler machinery, never declared (emit-cpp rule)
    // Reuse an existing decl for the same subprogram.
    if (SP) {
      if (clang::CXXMethodDecl *MD = methodFor(SP))
        return MD;
    }
    clang::QualType MT = functionType(F);
    bool IsConst = SP ? isConstMethod(SP) : false;
    clang::QualType Ret;
    SmallVector<clang::QualType, 8> Params;
    splitMethodType(MT, Ret, Params);
    clang::DeclarationName DN = methodName(RD, MethodName);
    auto *MD = clang::CXXMethodDecl::Create(
        Ctx.ast(), RD, clang::SourceLocation(),
        clang::DeclarationNameInfo(DN, clang::SourceLocation()),
        Ctx.ast().getFunctionType(Ret, Params,
                                  clang::FunctionProtoType::ExtProtoInfo()),
        Ctx.ast().getTrivialTypeSourceInfo(Ctx.ast().getFunctionType(
            Ret, Params, clang::FunctionProtoType::ExtProtoInfo())),
        clang::SC_None,
        /*UsesFPIntrin=*/false, /*isInline=*/false,
        clang::ConstexprSpecKind::Unspecified, clang::SourceLocation());
    MD->setAccess(clang::AS_public);
    if (SP && SP->getVirtuality() != 0)
      MD->setVirtualAsWritten(true);
    bindParams(MD, Params);
    RD->addDecl(MD);
    if (SP)
      MethodDecls[SP] = MD;
    if (!RD->isCompleteDefinition())
      RD->completeDefinition();
    (void)IsConst;
    return MD;
  }

  clang::DeclarationName methodName(clang::CXXRecordDecl *RD,
                                    const std::string &MethodName) {
    StringRef MN(MethodName);
    if (MN.starts_with('~') && MN.drop_front() == RD->getName())
      return Ctx.ast().DeclarationNames.getCXXDestructorName(
          Ctx.ast().getCanonicalType(Ctx.ast().getCanonicalTagType(RD)));
    if (MN == RD->getName())
      return Ctx.ast().DeclarationNames.getCXXConstructorName(
          Ctx.ast().getCanonicalType(Ctx.ast().getCanonicalTagType(RD)));
    // operator spellings: map the common ones to overloaded-operator names.
    if (MN.starts_with("operator")) {
      clang::OverloadedOperatorKind Op = clang::OO_None;
      StringRef Rest = MN.drop_front(sizeof("operator") - 1).trim();
      if (Rest == "==")
        Op = clang::OO_EqualEqual;
      else if (Rest == "!=")
        Op = clang::OO_ExclaimEqual;
      else if (Rest == "=")
        Op = clang::OO_Equal;
      else if (Rest == "+")
        Op = clang::OO_Plus;
      else if (Rest == "-")
        Op = clang::OO_Minus;
      else if (Rest == "*")
        Op = clang::OO_Star;
      else if (Rest == "/")
        Op = clang::OO_Slash;
      else if (Rest == "[]")
        Op = clang::OO_Subscript;
      else if (Rest == "()")
        Op = clang::OO_Call;
      else if (Rest == "new")
        Op = clang::OO_New;
      else if (Rest == "delete")
        Op = clang::OO_Delete;
      if (Op != clang::OO_None)
        return Ctx.ast().DeclarationNames.getCXXOperatorName(Op);
    }
    return clang::DeclarationName(Ctx.ident(MethodName));
  }

  clang::CXXMethodDecl *buildMethod(clang::CXXRecordDecl *RD,
                                    const std::string &MethodName,
                                    const DISubprogram *SP) {
    clang::QualType Ret = voidTy();
    SmallVector<clang::QualType, 8> Params;
    if (const auto *Sig = dyn_cast_or_null<DISubroutineType>(SP->getType())) {
      bool First = true;
      for (DIType *T : Sig->getTypeArray()) {
        if (First) {
          First = false;
          if (T)
            Ret = mapDIOrFallback(T);
          continue;
        }
        // Skip the object-pointer (`this`, FlagObjectPointer) param: it
        // is implicit in Clang.
        if (T && T->isObjectPointer())
          continue;
        Params.push_back(T ? mapDIOrFallback(T) : uintptrT());
      }
    }
    clang::FunctionProtoType::ExtProtoInfo EPI;
    clang::QualType Fn = Ctx.ast().getFunctionType(Ret, Params, EPI);
    clang::DeclarationName DN = methodName(RD, MethodName);
    auto *MD = clang::CXXMethodDecl::Create(
        Ctx.ast(), RD, clang::SourceLocation(),
        clang::DeclarationNameInfo(DN, clang::SourceLocation()), Fn,
        Ctx.ast().getTrivialTypeSourceInfo(Fn), clang::SC_None,
        /*UsesFPIntrin=*/false, /*isInline=*/false,
        clang::ConstexprSpecKind::Unspecified, clang::SourceLocation());
    MD->setAccess(clang::AS_public);
    if (SP->getVirtuality() != 0)
      MD->setVirtualAsWritten(true);
    bindParams(MD, Params);
    RD->addDecl(MD);
    MethodDecls[SP] = MD;
    return MD;
  }

  static bool isObjectPointerParam(const DISubprogram *SP) {
    if (const auto *Sig = dyn_cast_or_null<DISubroutineType>(SP->getType())) {
      for (DIType *T : Sig->getTypeArray()) {
        if (T)
          return T->isObjectPointer();
        return false;
      }
    }
    return false;
  }

  /// Const method: the object pointer is `const T*`. Port of emit-cpp's
  /// `this_const` (`const` qualifier on the definition).
  static bool isConstMethod(const DISubprogram *SP) {
    if (const auto *Sig = dyn_cast_or_null<DISubroutineType>(SP->getType())) {
      for (DIType *T : Sig->getTypeArray()) {
        if (const auto *DD = dyn_cast_or_null<DIDerivedType>(T)) {
          if (DD->getTag() == dwarf::DW_TAG_pointer_type) {
            if (const auto *Base =
                    dyn_cast_or_null<DIDerivedType>(DD->getBaseType()))
              return Base->getTag() == dwarf::DW_TAG_const_type;
          }
        }
        return false;
      }
    }
    return false;
  }

  void splitMethodType(clang::QualType MT, clang::QualType &Ret,
                       SmallVectorImpl<clang::QualType> &Params) {
    if (const auto *FP = dyn_cast<clang::FunctionProtoType>(MT.getTypePtr())) {
      Ret = FP->getReturnType();
      for (unsigned I = 0; I < FP->getNumParams(); ++I) {
        // Drop a leading `this`-shaped param (pointer to the record):
        // Clang methods carry it implicitly.
        if (I == 0 && FP->getParamType(0)->isPointerType())
          continue;
        Params.push_back(FP->getParamType(I));
      }
      return;
    }
    if (const auto *PT = dyn_cast<clang::PointerType>(MT.getTypePtr())) {
      if (const auto *FP =
              dyn_cast<clang::FunctionProtoType>(PT->getPointeeType())) {
        Ret = FP->getReturnType();
        for (unsigned I = 0; I < FP->getNumParams(); ++I) {
          if (I == 0 && FP->getParamType(0)->isPointerType())
            continue;
          Params.push_back(FP->getParamType(I));
        }
        return;
      }
    }
    Ret = voidTy();
  }

  void bindParams(clang::CXXMethodDecl *MD, ArrayRef<clang::QualType> Params) {
    SmallVector<clang::ParmVarDecl *, 8> Ps;
    for (unsigned I = 0; I < Params.size(); ++I) {
      std::string N = "a" + std::to_string(I + 1);
      auto *P = clang::ParmVarDecl::Create(
          Ctx.ast(), MD, clang::SourceLocation(), clang::SourceLocation(),
          Ctx.ident(N), Params[I],
          Ctx.ast().getTrivialTypeSourceInfo(Params[I]), clang::SC_None,
          nullptr);
      Ps.push_back(P);
    }
    MD->setParams(Ps);
  }

  // -- virtual dispatch ----------------------------------------------------------

  /// Scan DICompositeType elements for the vtable slot: entries carrying a
  /// virtual index matching `Slot` resolve to their DISubprogram. Matches
  /// emit-cpp's `virtuals: offset -> method` map (offsets in bytes; slots
  /// are pointer-sized, so slot N == byte offset N*8 on x86-64).
  const DISubprogram *virtualFromElements(const DICompositeType *RecordDI,
                                          unsigned Slot) {
    uint64_t WantBytes = static_cast<uint64_t>(Slot) * 8;
    {
      // First pass: exact byte-offset match on virtual members.
      for (DINode *El : RecordDI->getElements()) {
        const auto *SP = dyn_cast<DISubprogram>(El);
        if (!SP || SP->getVirtuality() == 0)
          continue;
        if (SP->getVirtualIndex() == WantBytes ||
            SP->getVirtualIndex() == Slot)
          return SP;
      }
      // Second pass: slot order among virtuals (vtable layout order == the
      // declaration order of virtuals in the element list).
      unsigned Idx = 0;
      for (DINode *El : RecordDI->getElements()) {
        const auto *SP = dyn_cast<DISubprogram>(El);
        if (!SP || SP->getVirtuality() == 0)
          continue;
        if (Idx++ == Slot)
          return SP;
      }
      // Walk bases (virtuals merge from base classes, like emit-cpp).
      for (DINode *El : RecordDI->getElements()) {
        const auto *DD = dyn_cast<DIDerivedType>(El);
        if (!DD || DD->getTag() != dwarf::DW_TAG_inheritance)
          continue;
        if (const auto *BC =
                dyn_cast_or_null<DICompositeType>(DD->getBaseType()))
          if (const DISubprogram *SP = virtualFromElements(BC, Slot))
            return SP;
      }
    }
    return nullptr;
  }

  /// Resolve through a registered vtable global: slot -> function value ->
  /// DISubprogram (or demangled method name matched to a materialized
  /// method). Mirrors emit-cpp `try_virtual`: `(*(*recv + k))(recv, ...)`
  /// resolves via the receiver's class vtable.
  const DISubprogram *virtualFromVTableGlobal(const DICompositeType *RecordDI,
                                              unsigned Slot) {
    StringRef Name = RecordDI->getName();
    if (Name.empty())
      Name = RecordDI->getIdentifier();
    auto It = VTables.find(std::string(Name));
    if (It == VTables.end())
      return nullptr;
    GlobalVariable *GV = It->second;
    const auto *Arr = dyn_cast_or_null<ConstantArray>(GV->getInitializer());
    if (!Arr || Slot >= Arr->getNumOperands())
      return nullptr;
    Value *Op = Arr->getOperand(Slot)->stripPointerCasts();
    const auto *F = dyn_cast<Function>(Op);
    if (!F)
      return nullptr;
    if (DISubprogram *SP = F->getSubprogram())
      return SP;
    return nullptr;
  }
};

std::unique_ptr<TypeBuilder> createTypeBuilder(FrontendContext &Ctx) {
  return std::make_unique<RealTypeBuilderImpl>(Ctx);
}

} // namespace revenant::clangx
