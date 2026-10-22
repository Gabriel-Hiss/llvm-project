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

  # Pass libs must build STATIC in the superbuild so revenant-opt links
  # them statically. A pass that builds MODULE/SHARED (plugin DLL) puts
  # the DLL path on the link line and lld-link rejects it; the fix is on
  # the pass side (BUILD_SHARED_LIBS OFF / STATIC). This stanza only
  # lists targets that exist at configure time.
  LINK_LIBS
  RevenantTransformLiftNormalize
  $<$<TARGET_EXISTS:RevenantTransformDevectorize>:RevenantTransformDevectorize>
  $<$<TARGET_EXISTS:RevenantTransformReroll>:RevenantTransformReroll>
  $<$<TARGET_EXISTS:RevenantTransformUnrotate>:RevenantTransformUnrotate>
  $<$<TARGET_EXISTS:RevenantTransformIdioms>:RevenantTransformIdioms>
  $<$<TARGET_EXISTS:RevenantTransformGsCookie>:RevenantTransformGsCookie>
  $<$<TARGET_EXISTS:RevenantTransformRustBlocks>:RevenantTransformRustBlocks>
  $<$<TARGET_EXISTS:RevenantTransformPatterns>:RevenantTransformPatterns>
  $<$<TARGET_EXISTS:RevenantStructure>:RevenantStructure>
  $<$<TARGET_EXISTS:RevenantTransformTypeRecovery>:RevenantTransformTypeRecovery>
  $<$<TARGET_EXISTS:RevenantTransformSsaOut>:RevenantTransformSsaOut>
  $<$<TARGET_EXISTS:RevenantTransformLocalSlots>:RevenantTransformLocalSlots>
  $<$<TARGET_EXISTS:RevenantTransformUnabi>:RevenantTransformUnabi>
  $<$<TARGET_EXISTS:RevenantTransformUnhoist>:RevenantTransformUnhoist>
  $<$<TARGET_EXISTS:RevenantTransformUninline>:RevenantTransformUninline>
  $<$<TARGET_EXISTS:RevenantTransformUnhotCold>:RevenantTransformUnhotCold>
  $<$<TARGET_EXISTS:RevenantTransformUnselect>:RevenantTransformUnselect>
  $<$<TARGET_EXISTS:RevenantTransformUntailDup>:RevenantTransformUntailDup>
  $<$<TARGET_EXISTS:RevenantTransformUneh>:RevenantTransformUneh>
  $<$<TARGET_EXISTS:RevenantTransformUnloopVec>:RevenantTransformUnloopVec>
  $<$<TARGET_EXISTS:RevenantTransformUnvecSearch>:RevenantTransformUnvecSearch>
  $<$<TARGET_EXISTS:RevenantTransformUnsroa>:RevenantTransformUnsroa>
  $<$<TARGET_EXISTS:RevenantTransformUnrust>:RevenantTransformUnrust>
  $<$<TARGET_EXISTS:RevenantTransformUnipo>:RevenantTransformUnipo>
  $<$<TARGET_EXISTS:RevenantTransformUncxx>:RevenantTransformUncxx>
  $<$<TARGET_EXISTS:RevenantTransformUniv>:RevenantTransformUniv>
  $<$<TARGET_EXISTS:RevenantTransformUnswitch>:RevenantTransformUnswitch>
  $<$<TARGET_EXISTS:RevenantTransformUnoutline>:RevenantTransformUnoutline>
  )
