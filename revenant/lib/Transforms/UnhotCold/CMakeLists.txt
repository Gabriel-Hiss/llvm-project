cmake_minimum_required(VERSION 3.20)

# Slice build, two modes (same convention as Devectorize):
# - Standalone (this directory is the top-level project): builds the `opt`
#   plugin DLL `RevenantTransformUnhotCold.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-unhotcold`. Configure
#   from a vcvars64 shell, e.g.:
#     cmake -G Ninja -S <worktree>/revenant/lib/Transforms/UnhotCold
#       -B <worktree>/build-rv-unhotcold
#       -DLLVM_DIR=C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm
#       -DCMAKE_CXX_COMPILER="<VS>/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#       -DLLVM_USE_LINKER=lld -DCMAKE_BUILD_TYPE=Release
#       "-DCMAKE_RC_COMPILER=C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe"
#       "-DCMAKE_MT=<VS>/VC/Tools/Llvm/x64/bin/llvm-mt.exe"
#   then `ninja -j4`.
# - Superbuild (`revenant/` top level, owner: RvSkeleton, via
#   add_subdirectory): builds a static library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-unhotcold` name
#   registration in Pipeline/Register.cpp.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(RevenantUnhotCold CXX)
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

set(UNHOTCOLD_SOURCES UnhotCold.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUnhotCold STATIC ${UNHOTCOLD_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
else()
  add_library(RevenantTransformUnhotCold MODULE ${UNHOTCOLD_SOURCES})
  set_target_properties(RevenantTransformUnhotCold PROPERTIES
    OUTPUT_NAME "RevenantTransformUnhotCold"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Core Support Analysis)
  target_link_libraries(RevenantTransformUnhotCold PRIVATE ${LLVM_LIBS})
  target_compile_definitions(RevenantTransformUnhotCold PRIVATE REVENANT_BUILD_PLUGIN)
  target_include_directories(RevenantTransformUnhotCold PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformUnhotCold PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUnhotCold PROPERTY CXX_STANDARD_REQUIRED ON)
