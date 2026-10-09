add_library(RevenantTransformUnrotate MODULE
  Unrotate.cpp
  UntailRec.cpp
  UnrotatePlugin.cpp
  )
llvm_map_components_to_libnames(LLVM_LIBS Analysis Core Passes Support)
target_link_libraries(RevenantTransformUnrotate PRIVATE ${LLVM_LIBS})
target_include_directories(RevenantTransformUnrotate PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../../../include)
add_dependencies(RevenantTransformUnrotate intrinsics_gen)
