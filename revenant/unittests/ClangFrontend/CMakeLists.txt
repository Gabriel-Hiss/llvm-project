revenant_add_unittest(RevenantUnitTests StructuredStmtBuilderTests
  StructuredStmtBuilderTest.cpp
  TranslationUnitTest.cpp
  ${CMAKE_SOURCE_DIR}/frontend/clang/lib/Expr/tests/ExprBuilderTests.cpp
  )

target_link_libraries(StructuredStmtBuilderTests PRIVATE RevenantClangCore RevenantIR)
target_include_directories(StructuredStmtBuilderTests PRIVATE
  ${CMAKE_SOURCE_DIR}/frontend/clang/include
  ${CMAKE_SOURCE_DIR}/frontend/clang/lib/Expr)

clang_target_link_libraries(StructuredStmtBuilderTests PRIVATE
  clangAST
  clangBasic
  )
