add_custom_target(RevenantUnitTests)
set_target_properties(RevenantUnitTests PROPERTIES FOLDER "Revenant tests")

# Each area adds its own suite under unittests/<Area>/.
foreach(dir Core Object Disasm Analysis Lift Transforms Structure Export)
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${dir} AND
     EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
