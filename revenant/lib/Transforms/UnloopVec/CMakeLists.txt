cmake_minimum_required(VERSION 3.20)

# Slice build, two modes:
# - Standalone (this directory is the top-level project): builds the `opt`
#   plugin DLL `RevenantTransformUnloopVec.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-unloopvec`. Configure with:
#     cmake -G Ninja -S <worktree>/revenant/lib/Transforms/UnloopVec
#       -B <worktree>/build-rv-unloopvec
#   then `ninja -j4`.
# - Superbuild (`revenant/` top level, owner: RvSkeleton, via
#   add_subdirectory): builds a static library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-unloopvec` name
#   registration in Pipeline/Register.cpp.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(RevenantUnloopVec CXX)
  find_package(LLVM REQUIRED CONFIG
    PATHS "C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm"
    NO_DEFAULT_PATH)
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

set(UNLOOPVEC_SOURCES UnloopVec.cpp UnvecMaps.cpp)
if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUnloopVec STATIC ${UNLOOPVEC_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
else()
  add_library(RevenantTransformUnloopVec MODULE ${UNLOOPVEC_SOURCES})
  set_target_properties(RevenantTransformUnloopVec PROPERTIES
    OUTPUT_NAME "RevenantTransformUnloopVec"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis)
  target_link_libraries(RevenantTransformUnloopVec PRIVATE ${LLVM_LIBS})
  target_compile_definitions(RevenantTransformUnloopVec PRIVATE REVENANT_BUILD_PLUGIN)
  target_include_directories(RevenantTransformUnloopVec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformUnloopVec PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUnloopVec PROPERTY CXX_STANDARD_REQUIRED ON)
