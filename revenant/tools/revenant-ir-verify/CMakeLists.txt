add_llvm_tool(revenant-ir-verify
  revenant-ir-verify.cpp
  )

target_link_libraries(revenant-ir-verify
  PRIVATE
  RevenantIR
  )
