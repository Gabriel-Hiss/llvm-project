//===- Factory.cpp - default builder construction --------------*- C++ -*-===//
//
// lib/Stmt, lib/Expr and lib/Type provide the real implementations of
// these factories. When those directories are absent CMake leaves the
// matching REVENANT_FRONTEND_EXTERNAL_* define unset and the Core fallback
// keeps the tool whole: goto bodies, literal/temporary expressions,
// IR-type-only QualTypes.
//
//===----------------------------------------------------------------------===//

#include "revenant/Clang/ExprBuilder.h"
#include "revenant/Clang/StmtBuilder.h"
#include "revenant/Clang/TypeBuilder.h"

namespace revenant::clangx {

#ifndef REVENANT_FRONTEND_EXTERNAL_STMT
std::unique_ptr<StmtBuilder> createStmtBuilder(FrontendContext &Ctx,
                                               TypeBuilder &Types) {
  return std::make_unique<GotoStmtBuilder>(Ctx, Types);
}
#endif

#ifndef REVENANT_FRONTEND_EXTERNAL_TYPE
std::unique_ptr<TypeBuilder> createTypeBuilder(FrontendContext &Ctx) {
  return std::make_unique<NullTypeBuilder>(Ctx);
}
#endif

} // namespace revenant::clangx
