# Revenant Clang Expr builder.

# Built from revenant/CMakeLists.txt; LLVM/Clang packages found there.

add_library(RevenantClangExpr STATIC
  ExprCore.cpp
  ExprScalar.cpp
  ExprMemory.cpp
  ExprCalls.cpp
  WideIntModel.cpp
)
target_include_directories(RevenantClangExpr PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../include
)
target_link_libraries(RevenantClangExpr PUBLIC
  clangAST
  clangBasic
  clangFrontend
  clangLex
  LLVMSupport
  LLVMCore
)
set_target_properties(RevenantClangExpr PROPERTIES CXX_STANDARD 17)
if(MSVC)
  target_compile_options(RevenantClangExpr PRIVATE /W4)
endif()

find_package(GTest QUIET)
if(TARGET llvm_gtest OR TARGET GTest::gtest)
  add_executable(RevenantClangExprTests
    tests/ExprBuilderTests.cpp
    tests/FrontendContextTestShim.cpp
  )
  if(TARGET llvm_gtest)
    target_link_libraries(RevenantClangExprTests PRIVATE
      RevenantClangExpr
      clangAST
      clangBasic
      clangFrontend
      clangLex
      LLVMSupport
      LLVMCore
      llvm_gtest
      llvm_gtest_main
    )
  else()
    target_link_libraries(RevenantClangExprTests PRIVATE
      RevenantClangExpr
      clangAST
      clangBasic
      clangFrontend
      clangLex
      LLVMSupport
      LLVMCore
      GTest::gtest
      GTest::gtest_main
    )
  endif()
  set_target_properties(RevenantClangExprTests PROPERTIES CXX_STANDARD 17)

endif()

# Test-only driver: run ExprBuilder over contract .ll modules. Unlike the
# gtests above it links no test framework, so it builds everywhere; keep it
# outside the GTest guard.
add_executable(RevenantExprContractHarness
  tests/ExprContractHarness.cpp
  tests/FrontendContextTestShim.cpp
)
target_link_libraries(RevenantExprContractHarness PRIVATE
  RevenantClangExpr
  clangAST
  clangBasic
  clangFrontend
  clangLex
  LLVMSupport
  LLVMCore
  LLVMAsmParser
)
set_target_properties(RevenantExprContractHarness PROPERTIES CXX_STANDARD 17)
