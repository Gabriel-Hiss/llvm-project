add_llvm_library(RevenantAnalysis
  Padding.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
  )
