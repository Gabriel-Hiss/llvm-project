add_llvm_library(RevenantTransformTypeRecovery
  TypeRecovery.cpp
  TypeInference.cpp

  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Support
  )
