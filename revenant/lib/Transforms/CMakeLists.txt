# Revenant transforms: one subdirectory per pass. Passes land
# independently; guard each one so missing slices don't break the build.
foreach(dir Devectorize Idioms Patterns Reroll SsaOut TypeRecovery Unrotate)
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${dir} AND
     EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
