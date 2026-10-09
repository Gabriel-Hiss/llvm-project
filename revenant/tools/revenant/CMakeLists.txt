add_llvm_tool(revenant
  revenant.cpp
  )

target_link_libraries(revenant PRIVATE
  RevenantObject
  RevenantDisasm
  )
