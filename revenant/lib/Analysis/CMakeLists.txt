add_llvm_library(RevenantAnalysis
  Noreturn.cpp
  Padding.cpp
  Xrefs.cpp
  Strings.cpp
  Resolver.cpp
  JumpTable.cpp
  StackPtr.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
  )
