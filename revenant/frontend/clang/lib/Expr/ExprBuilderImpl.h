//===- ExprBuilderImpl.h - internal ExprBuilder state -------*- C++ -*-===//
//
// Internal per-function state for the Revenant Clang ExprBuilder.
// Not installed; lib/Expr only.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_EXPRBUILDERIMPL_H
#define REVENANT_CLANG_EXPRBUILDERIMPL_H

#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/FrontendContext.h"
#include "revenant/Clang/TypeBuilder.h"
#include "revenant/Clang/WideIntModel.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "clang/AST/Stmt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"

namespace revenant::clangx {

// C operator precedence, higher binds tighter. Mirrors `prec` in
// crates/emit-c (and emit-cpp, which shares the table): postfix 17,
// unary/cast 15, mul 13, add 12, shift 11, relational 10, equality 9,
// & 8, ^ 7, | 6, ternary 4, && 3, || 2.
enum class Prec : unsigned {
  OrOr = 2,
  AndAnd = 3,
  Ternary = 4,
  Or = 6,
  Xor = 7,
  And = 8,
  Eq = 9,
  Rel = 10,
  Shift = 11,
  Add = 12,
  Mul = 13,
  Unary = 15,
  Postfix = 17,
  Atom = 18,
};

/// Concrete ExprBuilder; Core consumes its pending temporary declarations.
class ExprBuilderImpl final : public ExprBuilder {
public:
  ExprBuilderImpl(FrontendContext &Ctx, TypeBuilder &Types)
      : Ctx(Ctx), Types(Types) {}

  void beginFunction(llvm::Function &F, clang::FunctionDecl *FD,
                     ExprScope &Scope) override;
  clang::Expr *value(llvm::Value *V, ExprScope &Scope) override;
  clang::Stmt *instr(llvm::Instruction &I, ExprScope &Scope) override;
  clang::Expr *cond(llvm::Value *Cond, ExprScope &Scope) override;
  clang::Expr *retVal(llvm::Value *V, ExprScope &Scope) override;
  clang::Expr *inverted(llvm::Value *Cond, ExprScope &Scope) override;

  // Public for the unit tests: precedence of a built Expr, and the
  // literal-text rule for integer constants.
  static Prec precedenceOf(const clang::Expr *E);
  // Hex text for a mask constant (bitwise/shift operand), decimal
  // otherwise. Mirrors emit-c's `fmt` closure: only And/Or/Xor/Shl/Shr
  // force hex.
  static bool wantHexForOperand(clang::BinaryOperatorKind Op);

private:
  FrontendContext &Ctx;
  TypeBuilder &Types;
  clang::ASTContext *AST = nullptr;
  llvm::Function *CurF = nullptr;
  clang::FunctionDecl *CurFD = nullptr;
  // Instructions already materialized as `v<N>` temporaries (bound through
  // FrontendContext::bindTemp). value() resolves to the VarDecl ref.
  llvm::SmallPtrSet<llvm::Instruction *, 32> Materialized;
  // AllocaInst -> DILocalVariable for the current function, from
  // llvm.dbg.declare records.
  llvm::DenseMap<const llvm::Value *, const llvm::DIType *> DebugTypes;
  // This-pointer record DI for the vfptr-slot fast path, plus arg index.
  const llvm::DICompositeType *ThisRecord = nullptr;
  unsigned ThisArgIndex = 0;
  unsigned TempCounter = 0;

  clang::ASTContext &ast() const { return *AST; }
  clang::QualType uintptrTy() const;
  const llvm::DIType *debugTypeOf(const llvm::Value *V) const;
  // DI signedness for an integer value: DIBasicType DW_ATE_signed* or a
  // signed enumeral carrier means signed; unsigned encodings mean
  // unsigned. Nullptr (no DI) means signed, matching emit-c's default.
  bool isSignedIntValue(const llvm::Value *V) const;
  static bool isSignedDI(const llvm::DIType *DI);
  clang::QualType exprTypeOf(llvm::Value *V);
  clang::Expr *declRef(clang::ValueDecl *D);
  clang::Expr *tempFor(llvm::Instruction *I, ExprScope &Scope);
  clang::Expr *ensureTemp(llvm::Instruction *I, ExprScope &Scope,
                          clang::Expr *Inline);
  // Wrap E in a ParenExpr when its precedence is below Parent, or when it
  // equals Parent on the right of a non-associative operator.
  clang::Expr *parenIfNeeded(clang::Expr *E, Prec Parent,
                             bool RightOfNonAssoc = false);
  clang::Expr *buildIntegerLiteral(const llvm::APInt &V, clang::QualType Ty);
  clang::Expr *buildConstant(llvm::Constant *C, ExprScope &Scope);
  clang::Expr *buildBinary(llvm::Instruction &I, ExprScope &Scope);
  clang::Expr *buildCmp(llvm::CmpInst &I, ExprScope &Scope);
  clang::Expr *buildCast(llvm::CastInst &I, ExprScope &Scope);
  clang::Expr *buildLoadValue(llvm::LoadInst &I, ExprScope &Scope);
  clang::Expr *buildGEPValue(llvm::GetElementPtrInst &I, ExprScope &Scope);
  clang::Expr *buildSelect(llvm::SelectInst &I, ExprScope &Scope);
  clang::Expr *buildGEPFrom(clang::Expr *Base, llvm::Type *SrcElem,
                            llvm::Value *PtrOp,
                            llvm::ArrayRef<llvm::Value *> Idxs,
                            ExprScope &Scope);
  clang::Expr *buildCallValue(llvm::CallInst &I, ExprScope &Scope);
  // Cast the callee to the function type the call instruction itself
  // carries. Used when a prototype that returns void shadows the import
  // (system headers, recovered signatures), which would otherwise make
  // every value use of the call ill-formed.
  clang::Expr *castCalleeToCallType(llvm::CallInst &I, clang::Expr *Callee);
  clang::Expr *buildPHIValue(llvm::PHINode &I, ExprScope &Scope);
  clang::Expr *buildExtractValue(llvm::ExtractValueInst &I, ExprScope &Scope);
  clang::Stmt *buildStore(llvm::StoreInst &I, ExprScope &Scope);
  clang::Stmt *buildCallStmt(llvm::CallInst &I, ExprScope &Scope);
  clang::Expr *decayToPointer(clang::Expr *E, clang::QualType Pointee);
  clang::Expr *derefPointer(clang::Expr *Ptr, clang::QualType Pointee);
  clang::Expr *makeCStyleCast(clang::QualType T, clang::CastKind K,
                              clang::Expr *Op);
  clang::Expr *makeImplicit(clang::QualType T, clang::CastKind K,
                            clang::Expr *Op);
  clang::Expr *truthTest(clang::Expr *E);
  /// True when PtrOp is variable storage (alloca or global) and Ptr is
  /// its DeclRef: loads/stores use the name as the lvalue instead of
  /// dereferencing it. Pointer-typed arguments and computed pointers
  /// still dereference.
  static bool isStoredVar(llvm::Value *PtrOp, const clang::Expr *Ptr);
  // Unwrap a vtable-slot load chain to (object base value, slot index);
  // {nullptr, 0} when V is not such a chain. See the .cpp for shapes.
  std::pair<llvm::Value *, unsigned> vfptrBaseOf(llvm::Value *V);
  clang::Expr *tryVirtualCall(llvm::CallInst &I, ExprScope &Scope);
  clang::Expr *buildCallExpr(llvm::CallInst &I, ExprScope &Scope,
                             clang::Expr *Callee, clang::QualType RetTy);
  // Cast one call argument to the callee prototype's parameter type.
  clang::Expr *castCallArg(clang::Expr *E, clang::QualType Want);
  const llvm::DICompositeType *compositeBehindPointer(const llvm::DIType *DI);
  clang::FieldDecl *fieldAtOffset(clang::RecordDecl *Rec, uint64_t BitOff);
};

} // namespace revenant::clangx

#endif // REVENANT_CLANG_EXPRBUILDERIMPL_H
