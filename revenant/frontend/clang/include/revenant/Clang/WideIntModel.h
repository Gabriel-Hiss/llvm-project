//===- WideIntModel.h - IR integer widths C has no spelling for ---*- C++ -*-===//
//
// The corpus recompiles the emitted source with the producer's own
// compiler. MSVC's `cl` has no `__int128`, and MSVC C mode also rejects a
// cast to or from a struct type, so an IR `i128` value cannot be spelled
// with a 16-byte struct plus C-style casts alone.
//
// This module owns one representation that survives every construct the
// ExprBuilder emits on MSVC C mode: a two-field record plus compound
// literals. All conversion points funnel through `convert`, which decides
// the spelling from the source and destination types:
//
//   i128 -> i128 (same record) : the operand, uncast
//   i128 -> narrow            : (destination)operand.lo
//   narrow -> i128             : (record){operand, sign-fill-or-zero}
//   i128 -> other wide record   : (record){operand.lo, operand.hi}
//   i128 constant              : (record){lo, hi}, or (record){0, 0}
//
// Struct assignment is the same 16-byte move the x86 backend emits for a
// `store i128`, so `*(record *)dst = v;` keeps the lifted xmm image's
// semantics without inventing arithmetic.
//
// Arithmetic has no record spelling in C, so `arithmeticCall` routes every
// wide operator through a wrapper emitted once by `emitPreamble`. The
// wrappers are plain C: no compiler extensions, so the emitted text also
// compiles under clang-cl and gcc, which keeps one representation for
// every producer instead of a per-compiler spelling.
//
// GCC and clang-cl keep `__int128` where it already works; the record path
// only kicks in for a producer whose compiler rejects the keyword, so the
// corpus targets that use gcc are unaffected.
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_WIDEINTMODEL_H
#define REVENANT_CLANG_WIDEINTMODEL_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/OperationKinds.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/raw_ostream.h"

namespace revenant::clangx {

/// Integer widths modeled as a record. The IR only reaches 128 bits on
/// x86 (xmm registers are lifted as i128), so the list is closed.
inline constexpr unsigned WideIntBits = 128;

/// Construct (once) the record for `Bits`/`Signed`, register it on the
/// translation unit so Clang's printer emits the definition before any
/// function, and return its type. Returns a null QualType for a width the
/// model does not cover; callers fall back to their own mapping.
clang::QualType wideIntType(clang::ASTContext &A, unsigned Bits, bool Signed);

/// True when `T` is one of this module's records.
bool isWideIntRecord(clang::QualType T);

/// True when `T` is a wide record or a derived type (pointer, array,
/// elaborated) that eventually holds one. Used to decide whether a
/// declaration needs the definition emitted at all.
bool usesWideIntRecord(clang::QualType T);

/// Spell `Op` as type `Dst`, where at least one side is a wide record.
/// Returns nullptr when the pair is outside the model, so the caller can
/// apply its own cast rule.
clang::Expr *wideIntConvert(clang::ASTContext &A, clang::QualType Dst,
                            clang::Expr *Op, clang::CastKind K);

/// Spell `*(Dst) = Val` for a wide destination as a statement.
clang::Stmt *makeWideStore(clang::ASTContext &A, clang::Expr *Dst,
                           clang::Expr *Val);

/// Integer literal of type `T`, which must be a wide record.
clang::Expr *wideIntLiteral(clang::ASTContext &A, clang::QualType T,
                            const llvm::APInt &V);

/// Spell a wide binary operator or comparison as a call to one of the
/// wrappers `emitPreamble` prints. `Op` is the llvm opcode; for a
/// comparison it is `Instruction::ICmp`/`FCmp` and `Pred` selects. Returns
/// nullptr when the operator is outside the model.
clang::Expr *wideIntArithmetic(clang::ASTContext &A, clang::QualType ResTy,
                               unsigned Op, unsigned Pred, clang::Expr *LHS,
                               clang::Expr *RHS);

/// Print the arithmetic wrappers and record definitions the module needs.
/// Called once before the translation unit's decls are printed.
void emitWideIntPreamble(llvm::raw_ostream &OS, clang::ASTContext &A);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_WIDEINTMODEL_H
