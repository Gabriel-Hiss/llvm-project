add_llvm_library(RevenantTargetX86
  X86TargetHooks.cpp
  X86JumpTables.cpp
  LINK_COMPONENTS
  MC
  Support
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
  )
target_include_directories(RevenantTargetX86 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../llvm/lib/Target/X86"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../../llvm/lib/Target/X86/MCTargetDesc"
  "${LLVM_BINARY_DIR}/lib/Target/X86")
