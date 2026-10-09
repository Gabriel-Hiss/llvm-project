add_llvm_library(RevenantIR
  Contract.cpp

  LINK_COMPONENTS
  Core
  Analysis
  AsmParser
  Support
  )
