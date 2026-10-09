add_llvm_library(RevenantTransformLiftNormalize STATIC
  LiftNormalize.cpp
  LINK_COMPONENTS Core Support TransformUtils Analysis
)
