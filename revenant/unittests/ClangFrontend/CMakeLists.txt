revenant_add_unittest(RevenantUnitTests StructuredStmtBuilderTests
  StructuredStmtBuilderTest.cpp
  )

target_link_libraries(StructuredStmtBuilderTests PRIVATE RevenantClangCore)
target_include_directories(StructuredStmtBuilderTests PRIVATE
  ${CMAKE_SOURCE_DIR}/frontend/clang/include)

clang_target_link_libraries(StructuredStmtBuilderTests PRIVATE
  clangAST
  clangBasic
  )
