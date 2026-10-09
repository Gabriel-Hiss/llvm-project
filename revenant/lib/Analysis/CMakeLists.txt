add_llvm_library(RevenantAnalysis
  StackPtr.cpp

  LINK_COMPONENTS
  MC
  Support

  LINK_LIBS
  )
