add_llvm_library(RevenantAnalysis
  Padding.cpp
  Noreturn.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
)
