//===- RealTypeBuilder.h - concrete TypeBuilder interface ---*- C++ -*-===//
//
// Concrete extensions of revenant::clangx::TypeBuilder implemented in
// lib/Type/TypeBuilder.cpp. Core (LunaClangCore) includes this header to
// register vtable globals and recover methods from mangled names; the
// virtualMethod()/methodFor() interface in TypeBuilder.h is what
// ExprBuilder consumes per call.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_CLANG_REALTYPEBUILDER_H
#define REVENANT_CLANG_REALTYPEBUILDER_H

#include "revenant/Clang/TypeBuilder.h"

namespace llvm {
class GlobalVariable;
class Function;
} // namespace llvm

namespace clang {
class CXXRecordDecl;
} // namespace clang

namespace revenant::clangx {

// Concrete TypeBuilder with the C++ registration hooks.
class RealTypeBuilder : public TypeBuilder {
public:

  /// Recover method decls from a mangled function name (MSVC or Itanium
  /// via LLVM Demangle) and attach the method to its record.
  virtual clang::CXXMethodDecl *recoverMethod(llvm::Function &F) = 0;

  /// The `this` parameter type for a method of record `RD`.
  virtual clang::QualType thisType(clang::CXXRecordDecl *RD,
                                   bool IsConst) = 0;
};

} // namespace revenant::clangx

#endif // REVENANT_CLANG_REALTYPEBUILDER_H
