add_llvm_library(RevenantAnalysis
  Noreturn.cpp

  LINK_COMPONENTS
  MC
  Support

  LINK_LIBS
  )
