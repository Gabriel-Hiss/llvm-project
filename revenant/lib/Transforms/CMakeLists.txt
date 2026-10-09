# RevenantTransforms aggregator: builds every pass library that has landed.
#
# Each transform lives in its own library RevenantTransform<Name> under
# revenant/lib/Transforms/<Name>/, owned by its pass agent. This file only
# adds subdirectories that exist, so passes land independently: landing the
# directory + rebuilding is all it takes. The Pipeline library (this
# agent's) holds the registration site (Register.cpp) and the default
# pipeline text (Pipeline.cpp); it links every pass library that exists so
# revenant-opt resolves all landed `revenant-<name>` passes statically.

# Pass libraries first (Pipeline links them below).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Lane0/CMakeLists.txt)
  add_subdirectory(Lane0)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Devectorize/CMakeLists.txt)
  add_subdirectory(Devectorize)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Reroll/CMakeLists.txt)
  add_subdirectory(Reroll)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Unrotate/CMakeLists.txt)
  add_subdirectory(Unrotate)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Idioms/CMakeLists.txt)
  add_subdirectory(Idioms)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Patterns/CMakeLists.txt)
  add_subdirectory(Patterns)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Structure/CMakeLists.txt)
  add_subdirectory(Structure)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/TypeRecovery/CMakeLists.txt)
  add_subdirectory(TypeRecovery)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/Unswitch/CMakeLists.txt)
  add_subdirectory(Unswitch)
endif()

add_subdirectory(Pipeline)
