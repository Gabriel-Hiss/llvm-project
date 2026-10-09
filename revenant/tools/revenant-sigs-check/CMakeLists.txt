add_llvm_tool(revenant-sigs-check
  revenant-sigs-check.cpp
  )

target_link_libraries(revenant-sigs-check PRIVATE
  RevenantAnalysis
  RevenantDisasm
  RevenantObject
  RevenantTargetX86
  LLVMDebugInfoPDB
  )
