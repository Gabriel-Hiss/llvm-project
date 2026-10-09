# RevenantCAPI: the engine (Session/detect/pipeline/source emit) plus the C
# ABI in include/revenant-c/revenant.h. Slice libraries that exist are
# linked; missing ones get the in-engine fallbacks.
add_llvm_library(RevenantCAPI
  CAPI.cpp
  Detect.cpp
  Session.cpp
  AnalysisFallback.cpp
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
  )

# Optional slice libraries: link what is on the tree.
foreach(_opt
    RevenantAnalysis
    RevenantTargetX86
    RevenantLift
    RevenantTransformPipeline
    RevenantTransformIdioms
    RevenantTransformDevectorize
    RevenantTransformLane0
    RevenantTransformReroll
    RevenantTransformUnrotate
    RevenantTransformPatterns
    RevenantTransformTypeRecovery
    RevenantTransformSsaOut
    RevenantStructure
    RevenantClangCore)
  if(TARGET ${_opt})
    target_link_libraries(RevenantCAPI PRIVATE ${_opt})
  endif()
endforeach()

if(TARGET RevenantClangCore)
  target_compile_definitions(RevenantCAPI PRIVATE REVENANT_HAVE_CLANG_FRONTEND=1)
  target_include_directories(RevenantCAPI PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../frontend/clang/include)
endif()
