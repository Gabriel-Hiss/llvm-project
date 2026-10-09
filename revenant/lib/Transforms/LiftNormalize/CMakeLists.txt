add_llvm_library(RevenantTransformLiftNormalize STATIC
  LiftNormalize.cpp
  LiftABI.cpp
  LiftStructs.cpp
  LINK_COMPONENTS Core Support TransformUtils Analysis
  LINK_LIBS RevenantIR
)
