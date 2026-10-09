add_llvm_tool(revenant-decompile
  revenant-decompile.cpp
  )

target_link_libraries(revenant-decompile PRIVATE
  RevenantCAPI
  )
