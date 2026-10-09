cmake_minimum_required(VERSION 3.20)

# Slice build, two modes:
# - Standalone (this directory is the top-level project): builds the `opt`
#   plugin DLL `RevenantTransformUniv.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-univ`. Configure with:
#     cmake -G Ninja -S <worktree>/revenant/lib/Transforms/Univ
#       -B <worktree>/build-rv-univ
#   then `ninja -j4`.
# - Superbuild (`revenant/` top level, owner: RvSkeleton, via
#   add_subdirectory): builds a static library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-univ` name
#   registration in Pipeline/Register.cpp.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(RevenantUniv CXX)
  find_package(LLVM REQUIRED CONFIG)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  include_directories(${LLVM_INCLUDE_DIRS})
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
  add_definitions(${LLVM_DEFINITIONS_LIST})
  # Match the shared build: no exceptions, no RTTI.
  if(MSVC)
    add_compile_options(/GR- /EHs-c-)
  else()
    add_compile_options(-fno-exceptions -fno-rtti)
  endif()
endif()

set(UNIV_SOURCES Univ.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUniv STATIC ${UNIV_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
else()
  add_library(RevenantTransformUniv MODULE ${UNIV_SOURCES})
  set_target_properties(RevenantTransformUniv PROPERTIES
    OUTPUT_NAME "RevenantTransformUniv"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis)
  target_link_libraries(RevenantTransformUniv PRIVATE ${LLVM_LIBS})
  target_compile_definitions(RevenantTransformUniv PRIVATE REVENANT_BUILD_PLUGIN)
  target_include_directories(RevenantTransformUniv PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformUniv PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUniv PROPERTY CXX_STANDARD_REQUIRED ON)
