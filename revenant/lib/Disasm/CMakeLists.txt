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

target_include_directories(RevenantDisasm PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../llvm/lib/Target/X86"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../llvm/lib/Target/X86/MCTargetDesc"
  "${LLVM_BINARY_DIR}/lib/Target/X86")
