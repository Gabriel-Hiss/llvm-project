add_llvm_library(RevenantDisasm
  Disasm.cpp

  LINK_COMPONENTS
  MCDisassembler
  MC
  Object
  Support
  TargetParser
  X86AsmParser
  X86CodeGen
  X86Desc
  X86Disassembler
  X86Info

  LINK_LIBS
  )
