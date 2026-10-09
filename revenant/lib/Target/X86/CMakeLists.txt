add_llvm_library(RevenantTargetX86
  X86TargetHooks.cpp

  LINK_COMPONENTS
  MC
  Support
  X86Desc
  X86Info

  LINK_LIBS
  )
