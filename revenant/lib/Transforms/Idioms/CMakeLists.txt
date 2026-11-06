add_llvm_library(RevenantTransformIdioms
  Idioms.cpp

  LINK_COMPONENTS
  Analysis
  Core
  Support
  TransformUtils
  )
target_include_directories(RevenantTransformIdioms PUBLIC
  ${REVENANT_INCLUDE_DIR})
