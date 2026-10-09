add_llvm_library(RevenantLift
  LiftFunction.cpp
  LiftState.cpp
  LiftCalls.cpp
  LiftCore.cpp
  LiftX86.cpp

  LINK_COMPONENTS
  Analysis
  Core
  InstCombine
  MC
  MCDisassembler
  Passes
  ScalarOpts
  Support
  TargetParser
  TransformUtils
  X86Desc
  X86Disassembler
  X86Info
)

add_dependencies(RevenantLift RevenantDisasm RevenantIR)
target_link_libraries(RevenantLift PRIVATE RevenantDisasm RevenantIR)
# Private TableGen-descending X86 headers (X86MCTargetDesc.h, X86BaseInfo.h).
target_include_directories(RevenantLift PRIVATE
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86/MCTargetDesc"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
)

# Headers of sibling slices live in the same revenant/include tree.
target_include_directories(RevenantLift PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../../include"
)
