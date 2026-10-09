cmake_minimum_required(VERSION 3.20)

# Slice build, two modes:
# - Standalone (direct configure on this directory): builds the `opt`
#   plugin DLL `RevenantTransformUnsroa.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-unsroa`.
# - Superbuild (in-tree via add_subdirectory, `add_llvm_library`
#   available): builds a STATIC library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-unsroa` name
#   registration in Pipeline/Register.cpp. A MODULE/DLL must never reach
#   the superbuild link: lld-link takes the .dll as an input and the link
#   fails (same rule as Unrotate, commit 0876cfbba).
set(UNSROA_SOURCES Unsroa.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUnsroa STATIC ${UNSROA_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
  target_include_directories(RevenantTransformUnsroa PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
else()
  project(RevenantUnsroa CXX)

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

  add_library(RevenantTransformUnsroa MODULE
    ${UNSROA_SOURCES}
    UnsroaPlugin.cpp
    )
  if(WIN32)
    target_sources(RevenantTransformUnsroa PRIVATE Unsroa.def)
  endif()
  target_compile_definitions(RevenantTransformUnsroa PRIVATE REVENANT_BUILD_PLUGIN)
  set_target_properties(RevenantTransformUnsroa PROPERTIES
    OUTPUT_NAME "RevenantTransformUnsroa"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Analysis Core Passes Support)
  target_link_libraries(RevenantTransformUnsroa PRIVATE ${LLVM_LIBS})
  target_include_directories(RevenantTransformUnsroa PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../../include)
  if(TARGET intrinsics_gen)
    add_dependencies(RevenantTransformUnsroa intrinsics_gen)
  endif()
endif()
set_property(TARGET RevenantTransformUnsroa PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUnsroa PROPERTY CXX_STANDARD_REQUIRED ON)
