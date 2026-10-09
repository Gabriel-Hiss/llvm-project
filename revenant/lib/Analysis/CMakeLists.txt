add_llvm_library(RevenantAnalysis
  Xrefs.cpp
  Strings.cpp
  Resolver.cpp

  LINK_COMPONENTS
  MC
  Support

  LINK_LIBS
  RevenantDisasm
  )
