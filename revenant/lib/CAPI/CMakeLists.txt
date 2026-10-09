# RevenantCAPI: the engine (Session/detect/stage pipeline/source emit) plus
# the C ABI in include/revenant-c/revenant.h.
add_llvm_library(RevenantCAPI
  CAPI.cpp
  Detect.cpp
  Session.cpp
  Stages.cpp

  LINK_COMPONENTS
  Core
  IRReader
  Passes
  Support
  TargetParser

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
  ${CMAKE_CURRENT_SOURCE_DIR}/../../frontend/clang/include)
