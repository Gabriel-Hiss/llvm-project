cmake_minimum_required(VERSION 3.20)

# Slice build, two modes:
# - Standalone (this directory is the top-level project): builds the `opt`
#   plugin DLL `RevenantTransformDevectorize.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-devectorize`. Configure
#   from a vcvars64 shell, e.g.:
#     cmake -G Ninja -S <worktree>/revenant/lib/Transforms/Devectorize
#       -B <worktree>/build-rv-devec
#       -DLLVM_DIR=C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm
#       -DCMAKE_CXX_COMPILER="<VS>/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#       -DLLVM_USE_LINKER=lld -DCMAKE_BUILD_TYPE=Release
#       "-DCMAKE_RC_COMPILER=C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe"
#       "-DCMAKE_MT=<VS>/VC/Tools/Llvm/x64/bin/llvm-mt.exe"
#   then `ninja -j4`.
# - Superbuild (`revenant/` top level, owner: RvSkeleton, via
#   add_subdirectory): builds a static library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-devectorize` name
#   registration in Pipeline/Register.cpp.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(RevenantDevectorize CXX)
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

set(DEVEC_SOURCES Devectorize.cpp Lane0.cpp Propagate.cpp LoopDevec.cpp StraightDevec.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformDevectorize STATIC ${DEVEC_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
else()
  add_library(RevenantTransformDevectorize MODULE ${DEVEC_SOURCES})
  set_target_properties(RevenantTransformDevectorize PROPERTIES
    OUTPUT_NAME "RevenantTransformDevectorize"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis)
  target_link_libraries(RevenantTransformDevectorize PRIVATE ${LLVM_LIBS})
  target_compile_definitions(RevenantTransformDevectorize PRIVATE REVENANT_BUILD_PLUGIN)
  target_include_directories(RevenantTransformDevectorize PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformDevectorize PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformDevectorize PROPERTY CXX_STANDARD_REQUIRED ON)
