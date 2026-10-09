add_llvm_library(RevenantAnalysis
  Padding.cpp
  Noreturn.cpp
  Xrefs.cpp
  Strings.cpp
  Resolver.cpp
  JumpTable.cpp
  Candidates.cpp
  Overlaps.cpp
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
