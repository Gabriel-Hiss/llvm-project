add_llvm_library(RevenantAnalysis
  Padding.cpp
  Noreturn.cpp
  Signatures.cpp

  LINK_COMPONENTS
  MC
  Support
  TargetParser
  X86Desc
  X86Info

  LINK_LIBS
  RevenantDisasm
)
target_include_directories(RevenantAnalysis PRIVATE
  ${PROJECT_SOURCE_DIR}/../llvm/lib/Target/X86/MCTargetDesc
  ${LLVM_BINARY_DIR}/lib/Target/X86
  )
