# RevenantTransformPipeline: registration site + default pipeline text.
#
# Builds without any pass library present (every pass include in
# Register.cpp is __has_include-guarded) and links each pass library that
# has landed, so revenant-opt resolves all landed `revenant-<name>` passes
# statically. Pass agents own their own lib dirs; this file never changes
# when a pass lands.

add_llvm_library(RevenantTransformPipeline
  Pipeline.cpp
  Register.cpp

  LINK_COMPONENTS
  Core
  Passes
  Support

  LINK_LIBS
  $<$<TARGET_EXISTS:RevenantTransformLane0>:RevenantTransformLane0>
  $<$<TARGET_EXISTS:RevenantTransformDevectorize>:RevenantTransformDevectorize>
  $<$<TARGET_EXISTS:RevenantTransformReroll>:RevenantTransformReroll>
  $<$<TARGET_EXISTS:RevenantTransformUnrotate>:RevenantTransformUnrotate>
  $<$<TARGET_EXISTS:RevenantTransformIdioms>:RevenantTransformIdioms>
  $<$<TARGET_EXISTS:RevenantTransformPatterns>:RevenantTransformPatterns>
  $<$<TARGET_EXISTS:RevenantTransformStructure>:RevenantTransformStructure>
  $<$<TARGET_EXISTS:RevenantTransformTypeRecovery>:RevenantTransformTypeRecovery>
  $<$<TARGET_EXISTS:RevenantTransformSsaOut>:RevenantTransformSsaOut>
  $<$<TARGET_EXISTS:RevenantTransformUnabi>:RevenantTransformUnabi>
  )
