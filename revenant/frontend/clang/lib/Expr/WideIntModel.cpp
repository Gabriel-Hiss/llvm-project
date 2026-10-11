//===- WideIntModel.cpp - IR i128 -> a record MSVC accepts --------*- C++ -*-===//
//
// See WideIntModel.h for the representation and the conversion table. This
// file holds the record construction, the expression spellings, and the
// arithmetic wrappers that get printed into the emitted source.
//
// The wrappers were validated against a compiler-native 128-bit reference
// over 840k random cases plus boundary cases (shift by 0/63/64/65/127/128,
// INT128_MIN, full-width products, division by one); the differential test
// lives in the slice's scratch dir, not in this tree.

#include "revenant/Clang/WideIntModel.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

using clang::ASTContext;
using clang::CastKind;
using clang::Decl;
using clang::Expr;
using clang::QualType;
using clang::RecordDecl;
using clang::SourceLocation;
using llvm::StringRef;

namespace revenant::clangx {

namespace {

/// Naming: a fixed, non-colliding tag per width and signedness. The name
/// appears in the emitted source, so it should read like a generated
/// helper rather than an identifier the original program could carry.
llvm::SmallString<32> recordName(unsigned Bits, bool Signed) {
  llvm::SmallString<32> Name;
  llvm::raw_svector_ostream OS(Name);
  OS << "revenant_i" << Bits << (Signed ? "_s" : "_u");
  return Name;
}

/// Both halves of a wide record are the same width: a 128-bit value is two
/// 64-bit integers on the target data layout.
clang::QualType halfType(ASTContext &A, bool Signed) {
  return Signed ? A.LongLongTy : A.UnsignedLongLongTy;
}

bool isSigned(QualType Wide) {
  return llvm::cast<RecordDecl>(Wide->getAsRecordDecl())
      ->getName()
      .ends_with("_s");
}

RecordDecl *lookupRecord(ASTContext &A, unsigned Bits, bool Signed) {
  llvm::SmallString<32> Name = recordName(Bits, Signed);
  clang::IdentifierInfo &II = A.Idents.get(Name);
  for (clang::Decl *D : A.getTranslationUnitDecl()->lookup(&II))
    if (auto *RD = llvm::dyn_cast<RecordDecl>(D))
      if (RD->isCompleteDefinition())
        return RD;
  return nullptr;
}

/// Build the record, add it to the translation unit (NOT implicit: the
/// printer skips implicit decls, and the type name has to be spelled in
/// the casts), and complete it with two same-width fields.
RecordDecl *buildRecord(ASTContext &A, unsigned Bits, bool Signed) {
  llvm::SmallString<32> Name = recordName(Bits, Signed);
  clang::IdentifierInfo &II = A.Idents.get(Name);
  auto *TD = clang::RecordDecl::Create(
      A, clang::TagTypeKind::Struct, A.getTranslationUnitDecl(),
      SourceLocation(), SourceLocation(), &II);
  // Implicit, because the preamble prints the definition ahead of the
  // wrappers that use it. The decl pass skips implicit records.
  TD->setImplicit(true);
  A.getTranslationUnitDecl()->addDecl(TD);
  clang::QualType Half = halfType(A, Signed);
  const char *FieldNames[2] = {"lo", "hi"};
  for (unsigned I = 0; I != 2; ++I) {
    clang::IdentifierInfo &FII = A.Idents.get(FieldNames[I]);
    clang::FieldDecl *FD = clang::FieldDecl::Create(
        A, TD, SourceLocation(), SourceLocation(), &FII, Half, nullptr,
        nullptr, /*BitWidth=*/false, clang::ICIS_NoInit);
    TD->addDecl(FD);
  }
  TD->completeDefinition();
  return TD;
}

clang::FieldDecl *halfField(ASTContext &A, QualType Record, unsigned Index) {
  auto *RD = llvm::cast<RecordDecl>(Record->getAsRecordDecl());
  clang::IdentifierInfo *II =
      Index == 0 ? &A.Idents.get("lo") : &A.Idents.get("hi");
  for (clang::Decl *D : RD->lookup(clang::DeclarationName(II)))
    if (auto *FD = llvm::dyn_cast<clang::FieldDecl>(D))
      return FD;
  return nullptr;
}

/// A plain C-style cast, spelled exactly like ExprBuilderImpl's helper.
clang::Expr *cStyleCast(ASTContext &A, QualType T, CastKind K, Expr *Op) {
  clang::TypeSourceInfo *TSI = A.getTrivialTypeSourceInfo(T);
  return clang::CStyleCastExpr::Create(A, T, clang::VK_PRValue, K, Op, nullptr,
                                       clang::FPOptionsOverride(), TSI,
                                       SourceLocation(), SourceLocation());
}

clang::IntegerLiteral *intLiteral(ASTContext &A, uint64_t V, QualType T) {
  // The APInt width must match the type width exactly (Expr.cpp:968);
  // callers pass u64 fields and 32-bit counts through this helper.
  const unsigned W = (unsigned)A.getIntWidth(T);
  return clang::IntegerLiteral::Create(A, llvm::APInt(W, V), T,
                                       SourceLocation());
}


/// Low half read as a member access: `.lo` on the value the frontend
/// already built for the wide operand.
clang::Expr *lowHalf(ASTContext &A, QualType Record, Expr *Op) {
  return clang::MemberExpr::CreateImplicit(
      A, Op, /*IsArrow=*/false, halfField(A, Record, 0), Record,
      clang::VK_LValue, clang::OK_Ordinary);
}

/// `0 - (x < 0)`: -1 or 0, the sign fill for the signed record's high
/// half. A subtraction avoids the unary-operator node, whose creation
/// needs the ASTContext and an overflow flag for no gain here.
clang::Expr *signFill(ASTContext &A, Expr *Op) {
  clang::IntegerLiteral *Zero = intLiteral(A, 0, A.IntTy);
  clang::BinaryOperator *Test = clang::BinaryOperator::Create(
      A, Op, Zero, clang::BO_LT, A.IntTy, clang::VK_PRValue,
      clang::OK_Ordinary, SourceLocation(), clang::FPOptionsOverride());
  return clang::BinaryOperator::Create(
      A, Zero, Test, clang::BO_Sub, A.IntTy, clang::VK_PRValue,
      clang::OK_Ordinary, SourceLocation(), clang::FPOptionsOverride());
}

/// The operand as the record's unsigned low half, so a compound literal
/// initializes a field of the right width.
clang::Expr *srcUnsignedHalf(ASTContext &A, Expr *Op) {
  if (Op->getType() == A.UnsignedLongLongTy)
    return Op;
  return cStyleCast(A, A.UnsignedLongLongTy, clang::CK_IntegralCast, Op);
}

/// A call to a preamble wrapper: `fn(a, b)` with `ArgCount` operands.
clang::Expr *wrapperCall(ASTContext &A, clang::FunctionDecl *FD, Expr *A0,
                         Expr *A1 = nullptr) {
  llvm::SmallVector<Expr *, 2> Args{A0};
  if (A1)
    Args.push_back(A1);
  clang::DeclRefExpr *Callee = clang::DeclRefExpr::Create(
      A, clang::NestedNameSpecifierLoc(), SourceLocation(), FD,
      /*RefersToEnclosingVariableOrCapture=*/false, SourceLocation(),
      FD->getType(), clang::VK_LValue);
  return clang::CallExpr::Create(A, Callee, Args, FD->getReturnType(),
                                 clang::VK_PRValue, SourceLocation(),
                                 clang::FPOptionsOverride());
}

/// Build (once) the prototype for a wrapper named `Name` on record `Rec`.
/// Shift wrappers take a narrow `unsigned` count, matching the preamble.
clang::FunctionDecl *wrapperProto(ASTContext &A, QualType Rec, StringRef Name,
                                  bool Binary, bool Predicate,
                                  bool ShiftCount = false) {
  QualType Param = Rec;
  llvm::SmallVector<QualType, 2> Params;
  if (Predicate) {
    Params.push_back(Rec);
    Params.push_back(Rec);
  } else {
    Params.push_back(Param);
    if (Binary)
      Params.push_back(ShiftCount ? A.UnsignedIntTy : Param);
  }
  clang::FunctionProtoType::ExtProtoInfo EPI;
  QualType Ret = Predicate ? A.IntTy : Rec;
  clang::QualType FT = A.getFunctionType(Ret, Params, EPI);
  clang::IdentifierInfo &II = A.Idents.get(Name);
  clang::TranslationUnitDecl *TU = A.getTranslationUnitDecl();
  // Every same-name declaration added to the TU is compared against all
  // earlier ones, so a fresh prototype per call site made emission
  // quadratic in wide operations.
  for (clang::Decl *D : TU->lookup(clang::DeclarationName(&II)))
    if (auto *Old = llvm::dyn_cast<clang::FunctionDecl>(D);
        Old && Old->isImplicit() && Old->getType() == FT)
      return Old;
  auto *FD = clang::FunctionDecl::Create(
      A, TU, SourceLocation(), SourceLocation(), &II, FT,
      A.getTrivialTypeSourceInfo(Ret), clang::SC_Static);
  FD->setImplicit(true);
  TU->addDecl(FD);
  return FD;
}

/// Build a wide value from two u64 halves. In C, the only spelling is a
/// compound literal `(Record){lo, hi}`. In C++ (including MSVC C++), compound
/// literals are not valid (C4576), so call a preamble wrapper instead.
clang::Expr *makeWideValue(ASTContext &A, QualType T, Expr *Lo, Expr *Hi) {
  if (A.getLangOpts().CPlusPlus) {
    const bool Signed = isSigned(T);
    llvm::SmallString<64> Name(Signed ? "revenant_i128_s_make"
                                     : "revenant_i128_u_make");
    clang::FunctionDecl *FD =
        wrapperProto(A, T, Name, /*Binary=*/true, /*Predicate=*/false);
    return wrapperCall(A, FD, Lo, Hi);
  }
  // C path: compound literal.
  llvm::SmallVector<Expr *, 2> Inits{Lo, Hi};
  clang::InitListExpr *Init = new (A)
      clang::InitListExpr(A, SourceLocation(), Inits, SourceLocation());
  clang::TypeSourceInfo *TSI = A.getTrivialTypeSourceInfo(T);
  return new (A) clang::CompoundLiteralExpr(
      SourceLocation(), TSI, T, clang::VK_PRValue, Init,
      /*fileScope=*/false);
}

/// Return the signed variant of a wide record type. Used when dispatching
/// AShr: the opcode carries arithmetic semantics regardless of the record's
/// inferred signedness.
QualType makeSignedRecord(ASTContext &A, QualType T) {
  if (isSigned(T))
    return T;
  return wideIntType(A, WideIntBits, /*Signed=*/true);
}

} // namespace

clang::QualType wideIntType(ASTContext &A, unsigned Bits, bool Signed) {
  if (Bits != WideIntBits)
    return QualType();
  RecordDecl *TD = lookupRecord(A, Bits, Signed);
  if (!TD)
    TD = buildRecord(A, Bits, Signed);
  return A.getCanonicalTagType(TD);
}

bool isWideIntRecord(clang::QualType T) {
  if (T.isNull())
    return false;
  const auto *RD = T->getAsRecordDecl();
  if (!RD)
    return false;
  return RD->isCompleteDefinition() &&
         RD->getName().starts_with("revenant_i");
}

bool usesWideIntRecord(clang::QualType T) {
  if (T.isNull())
    return false;
  if (isWideIntRecord(T))
    return true;
  if (const auto *PT = T->getAs<clang::PointerType>())
    return usesWideIntRecord(PT->getPointeeType());
  if (const auto *AT = T->getAsArrayTypeUnsafe())
    return usesWideIntRecord(AT->getElementType());
  return false;
}

clang::Stmt *makeWideStore(clang::ASTContext &A, clang::Expr *Dst,
                            clang::Expr *Val) {
  Expr *Wide = wideIntConvert(A, Dst->getType(), Val, clang::CK_IntegralCast);
  return clang::BinaryOperator::Create(
      A, Dst, Wide, clang::BO_Assign, Dst->getType(), clang::VK_PRValue,
      clang::OK_Ordinary, SourceLocation(), clang::FPOptionsOverride());
}

clang::Expr *wideIntConvert(ASTContext &A, clang::QualType Dst, Expr *Op,
                            clang::CastKind K) {
  if (Dst.isNull() || Op == nullptr)
    return nullptr;
  const QualType Src = Op->getType();
  const bool DstWide = isWideIntRecord(Dst);
  const bool SrcWide = isWideIntRecord(Src);
  if (!DstWide && !SrcWide)
    return nullptr;

  // Same record: the IR's cast is a no-op, and a cast to the same struct
  // type is not legal C anyway.
  if (DstWide && SrcWide && Dst == Src)
    return Op;

  // wide -> wide of the other signedness: copy the halves across.
  if (DstWide && SrcWide)
    return makeWideValue(A, Dst, lowHalf(A, Src, Op),
                           clang::MemberExpr::CreateImplicit(
                               A, Op, false, halfField(A, Src, 1), Src,
                               clang::VK_LValue, clang::OK_Ordinary));

  // wide -> narrow: the low half carries the truncated value. Cast the
  // u64 field to the destination width so the literal bit width matches
  // the cast type (Expr.cpp:968 asserts exact width match).
  if (SrcWide) {
    Expr *Lo = lowHalf(A, Src, Op);
    if (Lo->getType() != Dst)
      Lo = cStyleCast(A, Dst, K, Lo);
    return Lo;
  }

  // narrow -> wide: the operand is the low half and the high half is the
  // sign or zero extension.
  Expr *Lo = srcUnsignedHalf(A, Op);
  Expr *Hi = isSigned(Dst) ? signFill(A, Op)
                           : intLiteral(A, 0, A.UnsignedLongLongTy);
  return makeWideValue(A, Dst, Lo, Hi);
}

clang::Expr *wideIntLiteral(ASTContext &A, clang::QualType T,
                            const llvm::APInt &V) {
  if (!isWideIntRecord(T))
    return nullptr;
  if (V.isZero()) {
    Expr *Zero = intLiteral(A, 0, A.UnsignedLongLongTy);
    return makeWideValue(A, T, Zero, Zero);
  }
  const uint64_t Lo = V.trunc(64).getZExtValue();
  const uint64_t Hi = V.lshr(64).trunc(64).getZExtValue();
  return makeWideValue(A, T, intLiteral(A, Lo, A.UnsignedLongLongTy),
                         intLiteral(A, Hi, A.UnsignedLongLongTy));
}

/// Wrapper suffix for a wide binary operator, or nullptr.
static const char *binaryWrapper(unsigned Op) {
  using namespace llvm;
  switch (Op) {
  case Instruction::Add: return "add";
  case Instruction::Sub: return "sub";
  case Instruction::Mul: return "mul";
  case Instruction::URem:
  case Instruction::SRem: return "rem";
  case Instruction::UDiv:
  case Instruction::SDiv: return "div";
  case Instruction::Shl: return "shl";
  case Instruction::LShr:
  case Instruction::AShr: return "shr";
  case Instruction::And: return "and";
  case Instruction::Or: return "or";
  case Instruction::Xor: return "xor";
  default: return nullptr;
  }
}

/// Wrapper suffix for a wide comparison, chosen from the IR predicate and
/// the operands' signedness. Signed and unsigned predicated map to
/// different wrappers because a record's fields carry the signedness.
static const char *compareWrapper(unsigned Pred, bool Signed) {
  using namespace llvm;
  switch (Pred) {
  case CmpInst::ICMP_EQ: return "cmpeq";
  case CmpInst::ICMP_NE: return "cmpne";
  case CmpInst::ICMP_UGT:
  case CmpInst::ICMP_SGT: return "cmpgt";
  case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_SGE: return "cmpge";
  case CmpInst::ICMP_ULT:
  case CmpInst::ICMP_SLT: return "cmplt";
  case CmpInst::ICMP_ULE:
  case CmpInst::ICMP_SLE: return "cmple";
  default: return nullptr;
  }
}

/// Spell `Op` so its type matches a wrapper's declared parameter `Want`.
/// A literal argument is rebuilt at the wanted width/signedness so a
/// constant spelled as u64 (the constant's own type) becomes the record
/// compound literal or the unsigned shift count the preamble declares;
/// a non-literal narrows through `.lo` or widens through the compound
/// literal/sign-fill paths of wideIntConvert.
clang::Expr *conformWideOperand(ASTContext &A, clang::QualType Want,
                                clang::Expr *Op) {
  if (Want.isNull() || Op == nullptr)
    return Op;
  if (Op->getType() == Want)
    return Op;
  if (auto *Lit = llvm::dyn_cast<clang::IntegerLiteral>(Op)) {
    if (isWideIntRecord(Want)) {
      if (clang::Expr *Wide =
              wideIntLiteral(A, Want, Lit->getValue()))
        return Wide;
    } else if (Want->isIntegerType()) {
      return clang::IntegerLiteral::Create(
          A, Lit->getValue().sextOrTrunc((unsigned)A.getIntWidth(Want)),
          Want, SourceLocation());
    }
  }
  if (clang::Expr *Wide =
          wideIntConvert(A, Want, Op, clang::CK_IntegralCast))
    return Wide;
  if (Want->isIntegerType() && Op->getType()->isIntegerType())
    return cStyleCast(A, Want, clang::CK_IntegralCast, Op);
  return Op;
}

clang::Expr *wideIntArithmetic(clang::ASTContext &A, clang::QualType ResTy,
                               unsigned Op, unsigned Pred, clang::Expr *LHS,
                               clang::Expr *RHS) {
  if (ResTy.isNull() || LHS == nullptr || RHS == nullptr)
    return nullptr;

  // A comparison: `Pred` selects the wrapper, both operands are wide.
  if (Op == (unsigned)llvm::Instruction::ICmp) {
    QualType OperandTy = LHS->getType();
    if (!isWideIntRecord(OperandTy))
      OperandTy = RHS->getType();
    if (!isWideIntRecord(OperandTy))
      return nullptr;
    const bool Signed = isSigned(OperandTy);
    const char *Suffix = compareWrapper(Pred, Signed);
    if (!Suffix)
      return nullptr;
    // If one operand is narrow (e.g. a u64 literal), convert it to the
    // wide record so the wrapper call type-checks.
    if (!isWideIntRecord(RHS->getType()))
      RHS = wideIntConvert(A, OperandTy, RHS, clang::CK_IntegralCast);
    llvm::SmallString<64> Name(recordName(WideIntBits, Signed));
    Name += "_";
    Name += Suffix;
    clang::FunctionDecl *FD =
        wrapperProto(A, OperandTy, Name, /*Binary=*/true, /*Predicate=*/true);
    // Constants and narrow operands conform to the record parameter.
    LHS = conformWideOperand(A, OperandTy, LHS);
    RHS = conformWideOperand(A, OperandTy, RHS);
    return wrapperCall(A, FD, LHS, RHS);
  }

  // A binary operator: the result type carries the wide record.
  if (!isWideIntRecord(ResTy))
    return nullptr;
  const char *Suffix = binaryWrapper(Op);
  if (!Suffix)
    return nullptr;
  const bool Signed = isSigned(ResTy);
  // AShr always uses the signed (arithmetic) helper. The opcode carries
  // arithmetic semantics; the record's inferred signedness is only a
  // spelling. Conform LHS to the signed record so the helper call
  // type-checks — the wide→wide halves copy is bit-exact.
  const bool IsArithmeticShift = Op == llvm::Instruction::AShr;
  QualType SignedTy = Signed ? ResTy : makeSignedRecord(A, ResTy);
  // Shifts: the second operand is a narrow count (unsigned int), not a
  // wide record. Do not convert it; the wrapper signature expects unsigned.
  const bool IsShift = Op == llvm::Instruction::Shl ||
                       Op == llvm::Instruction::LShr ||
                       Op == llvm::Instruction::AShr;
  if (!IsShift) {
    if (!isWideIntRecord(LHS->getType()))
      LHS = wideIntConvert(A, ResTy, LHS, clang::CK_IntegralCast);
    else if (!isWideIntRecord(RHS->getType()))
      RHS = wideIntConvert(A, ResTy, RHS, clang::CK_IntegralCast);
  } else {
    if (IsArithmeticShift && !Signed)
      LHS = wideIntConvert(A, SignedTy, LHS, clang::CK_IntegralCast);
    if (isWideIntRecord(RHS->getType()))
      RHS = wideIntConvert(A, A.UnsignedIntTy, RHS, clang::CK_IntegralCast);
    if (!isWideIntRecord(LHS->getType()))
      LHS = wideIntConvert(A, SignedTy, LHS, clang::CK_IntegralCast);
  }
  llvm::SmallString<64> Name(recordName(WideIntBits, IsArithmeticShift ? true : Signed));
  Name += "_";
  Name += Suffix;
  clang::FunctionDecl *FD =
      wrapperProto(A, IsArithmeticShift ? SignedTy : ResTy, Name, /*Binary=*/true, /*Predicate=*/false,
                   /*ShiftCount=*/IsShift);
  // Constants and narrow operands conform to the declared parameter
  // types: the record for value operands, `unsigned` for shift counts
  // (IsShift comes from the narrow-preserving block above).
  LHS = conformWideOperand(A, ResTy, LHS);
  RHS = conformWideOperand(A, IsShift ? A.UnsignedIntTy : ResTy, RHS);
  return wrapperCall(A, FD, LHS, RHS);
}

void emitWideIntPreamble(llvm::raw_ostream &OS, clang::ASTContext &A) {
  // Both spellings are plain C: no compiler extensions, so the same text
  // compiles under cl, clang-cl and gcc. Written out longhand rather than
  // templated on signedness, because the signed carry and sign-fill rules
  // differ in ways a shared template hid until it was wrong.
  static const char *Unsigned = R"CWRAP(
static unsigned long long revenant_hi64_u(unsigned long long a, unsigned long long b) {
  unsigned long long a0 = a & 0xffffffffULL, a1 = a >> 32;
  unsigned long long b0 = b & 0xffffffffULL, b1 = b >> 32;
  unsigned long long lolo = a0 * b0;
  unsigned long long mid = (lolo >> 32) + (a1 * b0 & 0xffffffffULL)
                         + (a0 * b1 & 0xffffffffULL);
  return a1 * b1 + (mid >> 32) + (a1 * b0 >> 32) + (a0 * b1 >> 32);
}

static struct revenant_i128_u revenant_i128_u_add(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r;
  r.lo = a.lo + b.lo;
  r.hi = a.hi + b.hi + (r.lo < a.lo);
  return r;
}

static struct revenant_i128_u revenant_i128_u_sub(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r;
  r.lo = a.lo - b.lo;
  r.hi = a.hi - b.hi - (a.lo < b.lo);
  return r;
}

static struct revenant_i128_u revenant_i128_u_mul(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r;
  r.lo = a.lo * b.lo;
  r.hi = revenant_hi64_u(a.lo, b.lo) + a.lo * b.hi + a.hi * b.lo;
  return r;
}

static struct revenant_i128_u revenant_i128_u_shl(struct revenant_i128_u a, unsigned n) {
  struct revenant_i128_u r;
  if (n == 0) return a;
  if (n >= 128) { r.lo = 0; r.hi = 0; return r; }
  if (n >= 64) { r.hi = a.lo << (n - 64); r.lo = 0; return r; }
  r.lo = a.lo << n;
  r.hi = (a.hi << n) | (a.lo >> (64 - n));
  return r;
}

static struct revenant_i128_u revenant_i128_u_shr(struct revenant_i128_u a, unsigned n) {
  struct revenant_i128_u r;
  if (n == 0) return a;
  if (n >= 128) { r.lo = 0; r.hi = 0; return r; }
  if (n >= 64) { r.lo = a.hi >> (n - 64); r.hi = 0; return r; }
  r.lo = (a.lo >> n) | (a.hi << (64 - n));
  r.hi = a.hi >> n;
  return r;
}

static int revenant_i128_u_cmpge(struct revenant_i128_u a, struct revenant_i128_u b) {
  if (a.hi != b.hi) return a.hi > b.hi;
  return a.lo >= b.lo;
}

static struct revenant_i128_u revenant_i128_u_and(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r; r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; return r;
}

static struct revenant_i128_u revenant_i128_u_or(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r; r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; return r;
}

static struct revenant_i128_u revenant_i128_u_xor(struct revenant_i128_u a, struct revenant_i128_u b) {
  struct revenant_i128_u r; r.lo = a.lo ^ b.lo; r.hi = a.hi ^ b.hi; return r;
}

static struct revenant_i128_u revenant_i128_u_restore(struct revenant_i128_u a, struct revenant_i128_u b, int quotient) {
  struct revenant_i128_u q, rem;
  int i;
  q.lo = 0; q.hi = 0; rem.lo = 0; rem.hi = 0;
  for (i = 127; i >= 0; --i) {
    unsigned bit = (i >= 64) ? (unsigned)((a.hi >> (i - 64)) & 1)
                             : (unsigned)((a.lo >> i) & 1);
    struct revenant_i128_u s = revenant_i128_u_shl(rem, 1);
    s.lo |= bit;
    if (revenant_i128_u_cmpge(s, b)) {
      rem = revenant_i128_u_sub(s, b);
      if (i < 64) q.lo |= 1ULL << i;
      else q.hi |= 1ULL << (i - 64);
    } else {
      rem = s;
    }
  }
  return quotient ? q : rem;
}

static struct revenant_i128_u revenant_i128_u_div(struct revenant_i128_u a, struct revenant_i128_u b) { return revenant_i128_u_restore(a, b, 1); }
static struct revenant_i128_u revenant_i128_u_rem(struct revenant_i128_u a, struct revenant_i128_u b) { return revenant_i128_u_restore(a, b, 0); }

static int revenant_i128_u_cmpgt(struct revenant_i128_u a, struct revenant_i128_u b) { return revenant_i128_u_cmpge(b, a) == 0 && !(a.hi == b.hi && a.lo == b.lo); }
static int revenant_i128_u_cmplt(struct revenant_i128_u a, struct revenant_i128_u b) { return revenant_i128_u_cmpge(a, b) == 0; }
static int revenant_i128_u_cmple(struct revenant_i128_u a, struct revenant_i128_u b) { return revenant_i128_u_cmpge(b, a) != 0; }
static int revenant_i128_u_cmpeq(struct revenant_i128_u a, struct revenant_i128_u b) { return a.hi == b.hi && a.lo == b.lo; }
static int revenant_i128_u_cmpne(struct revenant_i128_u a, struct revenant_i128_u b) { return !(a.hi == b.hi && a.lo == b.lo); }

static struct revenant_i128_u revenant_i128_u_make(unsigned long long lo, unsigned long long hi) {
  struct revenant_i128_u r; r.lo = lo; r.hi = hi; return r;
}
)CWRAP";

  static const char *Signed = R"CWRAP(
static unsigned long long revenant_hi64_s(unsigned long long a, unsigned long long b) {
  unsigned long long a0 = a & 0xffffffffULL, a1 = a >> 32;
  unsigned long long b0 = b & 0xffffffffULL, b1 = b >> 32;
  unsigned long long lolo = a0 * b0;
  unsigned long long mid = (lolo >> 32) + (a1 * b0 & 0xffffffffULL)
                         + (a0 * b1 & 0xffffffffULL);
  return a1 * b1 + (mid >> 32) + (a1 * b0 >> 32) + (a0 * b1 >> 32);
}

static long long revenant_i128_s_isneg(struct revenant_i128_s a) { return a.hi < 0; }

static struct revenant_i128_s revenant_i128_s_add(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r;
  /* The carry comes from an unsigned compare: a signed `r.lo < a.lo`
     reads a negative operand as already wrapped and drops the carry. */
  unsigned long long lo = (unsigned long long)a.lo + (unsigned long long)b.lo;
  r.lo = (long long)lo;
  r.hi = a.hi + b.hi + (lo < (unsigned long long)a.lo);
  return r;
}

static struct revenant_i128_s revenant_i128_s_sub(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r;
  unsigned long long lo = (unsigned long long)a.lo - (unsigned long long)b.lo;
  r.lo = (long long)lo;
  r.hi = a.hi - b.hi - ((unsigned long long)a.lo < (unsigned long long)b.lo);
  return r;
}

static struct revenant_i128_s revenant_i128_s_mul(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r;
  r.lo = (long long)((unsigned long long)a.lo * (unsigned long long)b.lo);
  r.hi = (long long)(revenant_hi64_s((unsigned long long)a.lo, (unsigned long long)b.lo)
                   + (unsigned long long)a.lo * (unsigned long long)b.hi
                   + (unsigned long long)a.hi * (unsigned long long)b.lo);
  return r;
}

static struct revenant_i128_s revenant_i128_s_shl(struct revenant_i128_s a, unsigned n) {
  struct revenant_i128_s r;
  if (n == 0) return a;
  if (n >= 128) { r.lo = 0; r.hi = 0; return r; }
  if (n >= 64) { r.hi = (long long)((unsigned long long)a.lo << (n - 64)); r.lo = 0; return r; }
  r.lo = (long long)((unsigned long long)a.lo << n);
  r.hi = (long long)(((unsigned long long)a.hi << n)
                   | ((unsigned long long)a.lo >> (64 - n)));
  return r;
}

static struct revenant_i128_s revenant_i128_s_neg(struct revenant_i128_s a) {
  struct revenant_i128_s r;
  r.lo = ~a.lo + 1;
  r.hi = ~a.hi + (a.lo == 0);
  return r;
}

static struct revenant_i128_s revenant_i128_s_shr(struct revenant_i128_s a, unsigned n) {
  struct revenant_i128_s r;
  if (n == 0) return a;
  if (n >= 128) { long long fill = a.hi < 0 ? -1 : 0; r.lo = fill; r.hi = fill; return r; }
  if (n >= 64) {
    unsigned k = n - 64;
    long long fill = a.hi < 0 ? -1 : 0;
    if (k == 0) { r.lo = a.hi; r.hi = fill; return r; }
    r.lo = (long long)(((unsigned long long)a.hi >> k)
                     | ((unsigned long long)fill << (64 - k)));
    r.hi = fill;
    return r;
  }
  r.lo = (long long)(((unsigned long long)a.lo >> n)
                   | ((unsigned long long)a.hi << (64 - n)));
  r.hi = a.hi >> n;
  return r;
}

static int revenant_i128_s_cmpge(struct revenant_i128_s a, struct revenant_i128_s b) {
  if (a.hi != b.hi) return a.hi > b.hi;
  return a.lo >= b.lo;
}

static struct revenant_i128_s revenant_i128_s_and(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r; r.lo = a.lo & b.lo; r.hi = a.hi & b.hi; return r;
}

static struct revenant_i128_s revenant_i128_s_or(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r; r.lo = a.lo | b.lo; r.hi = a.hi | b.hi; return r;
}

static struct revenant_i128_s revenant_i128_s_xor(struct revenant_i128_s a, struct revenant_i128_s b) {
  struct revenant_i128_s r; r.lo = a.lo ^ b.lo; r.hi = a.hi ^ b.hi; return r;
}

static struct revenant_i128_s revenant_i128_s_restore(struct revenant_i128_s a, struct revenant_i128_s b, int quotient) {
  struct revenant_i128_s q, rem;
  int i;
  int na = a.hi < 0, nb = b.hi < 0;
  q.lo = 0; q.hi = 0; rem.lo = 0; rem.hi = 0;
  if (na) a = revenant_i128_s_neg(a);
  if (nb) b = revenant_i128_s_neg(b);
  for (i = 127; i >= 0; --i) {
    unsigned bit = (i >= 64) ? (unsigned)((unsigned long long)a.hi >> (i - 64) & 1)
                             : (unsigned)((unsigned long long)a.lo >> i & 1);
    struct revenant_i128_s s = revenant_i128_s_shl(rem, 1);
    s.lo = (long long)((unsigned long long)s.lo | bit);
    if (revenant_i128_s_cmpge(s, b)) {
      rem = revenant_i128_s_sub(s, b);
      if (i < 64) q.lo |= 1LL << i;
      else q.hi |= 1LL << (i - 64);
    } else {
      rem = s;
    }
  }
  if (quotient && na != nb) q = revenant_i128_s_neg(q);
  if (!quotient && na) rem = revenant_i128_s_neg(rem);
  return quotient ? q : rem;
}

static struct revenant_i128_s revenant_i128_s_div(struct revenant_i128_s a, struct revenant_i128_s b) { return revenant_i128_s_restore(a, b, 1); }
static struct revenant_i128_s revenant_i128_s_rem(struct revenant_i128_s a, struct revenant_i128_s b) { return revenant_i128_s_restore(a, b, 0); }

static int revenant_i128_s_cmpgt(struct revenant_i128_s a, struct revenant_i128_s b) { return revenant_i128_s_cmpge(b, a) == 0 && !(a.hi == b.hi && a.lo == b.lo); }
static int revenant_i128_s_cmplt(struct revenant_i128_s a, struct revenant_i128_s b) { return revenant_i128_s_cmpge(a, b) == 0; }
static int revenant_i128_s_cmple(struct revenant_i128_s a, struct revenant_i128_s b) { return revenant_i128_s_cmpge(b, a) != 0; }
static int revenant_i128_s_cmpeq(struct revenant_i128_s a, struct revenant_i128_s b) { return a.hi == b.hi && a.lo == b.lo; }
static int revenant_i128_s_cmpne(struct revenant_i128_s a, struct revenant_i128_s b) { return !(a.hi == b.hi && a.lo == b.lo); }
static struct revenant_i128_s revenant_i128_s_make(long long lo, long long hi) {
  struct revenant_i128_s r; r.lo = lo; r.hi = hi; return r;
}
)CWRAP";

  OS << "struct revenant_i128_u { unsigned long long lo, hi; };\n";
  OS << "struct revenant_i128_s { long long lo, hi; };\n\n";
  OS << Unsigned;
  OS << Signed;
}

} // namespace revenant::clangx
