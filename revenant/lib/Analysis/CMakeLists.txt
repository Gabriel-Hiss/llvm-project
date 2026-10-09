add_llvm_library(RevenantAnalysis
  Analysis.cpp
  Builder.cpp
  Candidates.cpp
  JumpTable.cpp
  Noreturn.cpp
  Orphan.cpp
  Overlaps.cpp
  Padding.cpp
  ProgramAnalyze.cpp
  Resolver.cpp
  StackPtr.cpp
  Strings.cpp
  Xrefs.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
  )
