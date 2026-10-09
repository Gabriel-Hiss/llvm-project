if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(RevenantLocalSlots CXX)
  find_package(LLVM REQUIRED CONFIG
    PATHS "C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm"
    NO_DEFAULT_PATH)
  include_directories(${LLVM_INCLUDE_DIRS})
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
  add_definitions(${LLVM_DEFINITIONS_LIST})
  if(MSVC)
    add_compile_options(/GR- /EHs-c-)
  else()
    add_compile_options(-fno-exceptions -fno-rtti)
  endif()
endif()

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformLocalSlots STATIC
    LocalSlots.cpp
    LINK_COMPONENTS
    Core
    Support
    Analysis
    TransformUtils
    )
else()
  add_library(RevenantTransformLocalSlots MODULE LocalSlots.cpp)
  if(WIN32)
    target_sources(RevenantTransformLocalSlots PRIVATE LocalSlots.def)
  endif()
  target_compile_definitions(RevenantTransformLocalSlots PRIVATE
    REVENANT_BUILD_PLUGIN)
  set_target_properties(RevenantTransformLocalSlots PROPERTIES
    OUTPUT_NAME "RevenantTransformLocalSlots"
    PREFIX ""
    SUFFIX ".dll")
  llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis TransformUtils)
  target_link_libraries(RevenantTransformLocalSlots PRIVATE ${LLVM_LIBS})
  target_include_directories(RevenantTransformLocalSlots PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
endif()
set_property(TARGET RevenantTransformLocalSlots PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformLocalSlots PROPERTY CXX_STANDARD_REQUIRED ON)
