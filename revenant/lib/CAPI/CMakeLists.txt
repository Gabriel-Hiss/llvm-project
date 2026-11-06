# RevenantCAPI: the engine (Session/detect/stage pipeline/source emit) plus
# the C ABI in include/revenant-c/revenant.h.
add_llvm_library(RevenantCAPI
  CAPI.cpp
  Detect.cpp
  Session.cpp
  PairReturns.cpp
  Stages.cpp
  Unoptimized.cpp

  LINK_COMPONENTS
  Core
  IRReader
  MC
  Passes
  Support
  TargetParser
  TransformUtils
  LINK_LIBS
  RevenantObject
  RevenantDisasm
  RevenantIR
  RevenantAnalysis
  RevenantTargetX86
  RevenantLift
  RevenantTransformPipeline
  RevenantClangCore
)
target_include_directories(RevenantCAPI PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../frontend/clang/include"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86"
  "${LLVM_BINARY_DIR}/lib/Target/X86"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86/MCTargetDesc")
