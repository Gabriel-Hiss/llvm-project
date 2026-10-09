//===- TypeBuilder.h - LLVM/DI type -> clang::QualType ----------*- C++ -*-===//
//
// TypeBuilder is the frontend's type oracle. MuseClangCpp implements it
// (lib/Type/): it maps contract IR types and debug-info types onto Clang
// types, owns C++ record construction from DICompositeType (classes,
// vftables, bases), and answers the virtual-dispatch queries ExprBuilder
// needs. Core ships NullTypeBuilder so the frontend can emit a compilable
// skeleton before the real implementation lands.
//
// Convention: every lookup that cannot resolve a precise type returns
// "uintptr_t" (unsigned 64-bit) for scalar-ish slots and `void *` where a
// pointer is required, never an invalid QualType. Returning a null QualType
// is a bug; callers assert on it in debug builds.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_TYPEBUILDER_H
#define REVENANT_CLANG_TYPEBUILDER_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Type.h"
#include <memory>

namespace revenant::clangx {

class FrontendContext;

class TypeBuilder {
public:
  virtual ~TypeBuilder() = default;

  /// The Clang type for an LLVM IR type. `DI` is the debug-info type
  /// associated with the value when one exists (DILocalVariable::getType(),
  /// DISubprogramType elements, DIGlobalVariable::getType()); pass nullptr
  /// when the IR type is all we have.
  virtual clang::QualType typeOf(llvm::Type *Ty, const llvm::DIType *DI) = 0;

  /// Same, from a debug-info type alone (e.g. a field type or a typedef
  /// reached only through DI).
  virtual clang::QualType typeOfDI(const llvm::DIType *DI) = 0;

  /// The complete clang::FunctionType (return + params) for `F`, preferring
  /// `F.getSubprogram()->getType()` element types over the IR signature.
  virtual clang::QualType functionType(llvm::Function &F) = 0;

  /// The clang type a global's *storage* has: `GV.getValueType()` (a global
  /// is declared with the pointee type, not a pointer to it).
  virtual clang::QualType globalType(llvm::GlobalVariable &GV) = 0;

  /// Materialize (or return the cached) RecordDecl/CXXRecordDecl for a
  /// DICompositeType. Returns nullptr when `DI` is not a composite this
  /// builder handles; callers then treat the type opaquely.
  virtual clang::RecordDecl *recordFor(const llvm::DICompositeType *DI) = 0;

  /// Vftable hook for ExprBuilder: given the record DI behind a `this`
  /// pointer and a vftable slot index, return the DISubprogram that slot
  /// dispatches to (per DICompositeType::getVTableHolder() /
  /// DIDerivedType vftable entries). nullptr when unknown or not virtual.
  virtual const llvm::DISubprogram *
  virtualMethod(const llvm::DICompositeType *RecordDI, unsigned Slot) = 0;

  /// The Clang decl that `virtualMethod` resolved `SP` to, if it was
  /// already materialized (nullptr is fine; callers fall back to a
  /// function-pointer call through the vfptr slot).
  virtual clang::CXXMethodDecl *methodFor(const llvm::DISubprogram *SP) = 0;
};

/// Stub used by Core until lib/Type lands: integer widths map to
/// fixed-width types (unsigned unless DI says signed), floats to
/// float/double, pointers to `void *`, structs to anonymous byte arrays,
/// everything else to `uintptr_t`. Records resolve to nullptr.
class NullTypeBuilder final : public TypeBuilder {
public:
  explicit NullTypeBuilder(FrontendContext &Ctx);

  clang::QualType typeOf(llvm::Type *Ty, const llvm::DIType *DI) override;
  clang::QualType typeOfDI(const llvm::DIType *DI) override;
  clang::QualType functionType(llvm::Function &F) override;
  clang::QualType globalType(llvm::GlobalVariable &GV) override;
  clang::RecordDecl *recordFor(const llvm::DICompositeType *DI) override;
  const llvm::DISubprogram *
  virtualMethod(const llvm::DICompositeType *RecordDI, unsigned Slot) override;
  clang::CXXMethodDecl *methodFor(const llvm::DISubprogram *SP) override;
private:
  clang::QualType functionTypeFor(llvm::FunctionType *FTy,
                                  const llvm::DIType *DI);
  FrontendContext &Ctx;
};

/// Construct the real TypeBuilder implementation (defined in lib/Type/).
std::unique_ptr<TypeBuilder> createTypeBuilder(FrontendContext &Ctx);

} // namespace revenant::clangx

#endif // REVENANT_CLANG_TYPEBUILDER_H
