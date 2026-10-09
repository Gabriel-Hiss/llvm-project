add_llvm_tool(revenant-detect
  revenant-detect.cpp
  )

target_link_libraries(revenant-detect
  PRIVATE
  RevenantObject
  RevenantCompilerProfile
  )
