set(LLVM_LINK_COMPONENTS
  AllTargetsDescs
  AllTargetsDisassemblers
  AllTargetsInfos
  MC
  MCDisassembler
  Object
  Support
  TargetParser
  )

add_llvm_tool(llvm-revenant-opcount
  llvm-revenant-opcount.cpp
  )
