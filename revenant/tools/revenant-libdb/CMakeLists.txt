add_llvm_tool(revenant-libdb
  revenant-libdb.cpp
)

target_link_libraries(revenant-libdb PRIVATE
  RevenantAnalysis
  RevenantDisasm
  RevenantObject
  RevenantTargetX86
  LLVMObject
  LLVMDemangle
)
target_link_libraries(revenant-libdb PRIVATE
  clangAST
  clangBasic
  clangDriver
  clangFrontend
  clangLex
  clangOptions
  clangParse
  clangSema
  clangSerialization
)
target_link_libraries(revenant-libdb PRIVATE LLVMDebugInfoPDB)
