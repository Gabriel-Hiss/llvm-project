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

# X86BaseInfo.h exposes the MCInst prefix flag used by the X86 printer.
get_filename_component(REVENANT_LLVM_SOURCE_DIR "${LLVM_DIR}/../../../.." ABSOLUTE)
get_filename_component(REVENANT_LLVM_BUILD_DIR "${LLVM_DIR}/../../.." ABSOLUTE)
target_include_directories(RevenantDisasm PRIVATE
  "${REVENANT_LLVM_SOURCE_DIR}/llvm/lib/Target/X86/MCTargetDesc"
  "${REVENANT_LLVM_BUILD_DIR}/lib/Target/X86")
