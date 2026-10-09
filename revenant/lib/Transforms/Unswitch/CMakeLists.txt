if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
cmake_minimum_required(VERSION 3.20)
project(RevenantUnswitch CXX)

# Standalone slice build. Configured directly on this directory from a
# vcvars64 shell:
#   cmake -G Ninja -S <worktree>/revenant/lib/Transforms/Unswitch
#     -B <worktree>/build-rv-unswitch
#     -DLLVM_DIR=<build-revenant>/lib/cmake/llvm
#     -DCMAKE_BUILD_TYPE=Release
# then `ninja -j4`. The top-level `revenant/` superbuild (owner: RvSkeleton)
# and `revenant-opt` (owner: MuseTxPipeline) pick this directory up with
# add_subdirectory; keep this file self-sufficient for both uses.
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

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUnswitch
    Unswitch.cpp

    LINK_COMPONENTS
    Core
    Support
    TransformUtils
    )
  target_include_directories(RevenantTransformUnswitch PUBLIC
    ${REVENANT_INCLUDE_DIR})
else()
  add_library(RevenantTransformUnswitch MODULE
    Unswitch.cpp
  )
  if(WIN32)
    target_sources(RevenantTransformUnswitch PRIVATE Unswitch.def)
  endif()
  # Link against the shared build-tree libraries. llvm_map_components_to_libnames
  # resolves against the install prefix and misses these libraries.
  set(LLVM_BUILD_LIBDIR
    "C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib")
  target_link_libraries(RevenantTransformUnswitch PRIVATE
    ${LLVM_BUILD_LIBDIR}/LLVMCore.lib
    ${LLVM_BUILD_LIBDIR}/LLVMSupport.lib
    ${LLVM_BUILD_LIBDIR}/LLVMTransformUtils.lib
    ${LLVM_BUILD_LIBDIR}/LLVMBinaryFormat.lib
    ${LLVM_BUILD_LIBDIR}/LLVMRemarks.lib
    ${LLVM_BUILD_LIBDIR}/LLVMTargetParser.lib
    ${LLVM_BUILD_LIBDIR}/LLVMDemangle.lib
    ${LLVM_BUILD_LIBDIR}/LLVMBitstreamReader.lib
    ntdll.lib
  )
  target_include_directories(RevenantTransformUnswitch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
  )
  target_compile_definitions(RevenantTransformUnswitch PRIVATE REVENANT_BUILD_PLUGIN)
  set_property(TARGET RevenantTransformUnswitch PROPERTY CXX_STANDARD 17)
  set_property(TARGET RevenantTransformUnswitch PROPERTY CXX_STANDARD_REQUIRED ON)
  if(WIN32)
    set_target_properties(RevenantTransformUnswitch PROPERTIES
      OUTPUT_NAME "RevenantTransformUnswitch"
      PREFIX ""
      SUFFIX ".dll")
  endif()
endif()
