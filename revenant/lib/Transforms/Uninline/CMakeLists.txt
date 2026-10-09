cmake_minimum_required(VERSION 3.20)
project(RevenantUninline CXX)

# Standalone slice build. Configured directly on this directory from a
# vcvars64 shell:
#   cmake -G Ninja -S <worktree>/revenant/lib/Transforms/Uninline
#     -B <worktree>/build-rv-uninline
#     -DLLVM_DIR=C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm
#     -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#     -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#     -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe
#     -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe
#     -DLLVM_USE_LINKER=lld -DCMAKE_BUILD_TYPE=Release
# then `ninja -j4`. The top-level `revenant/` superbuild (owner: RvSkeleton)
# and `revenant-opt` (owner: MuseTxPipeline) pick this directory up with
# add_subdirectory; keep this file self-sufficient for both uses.
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

add_library(RevenantTransformUninline MODULE
  Uninline.cpp
)
if(WIN32)
  target_sources(RevenantTransformUninline PRIVATE Uninline.def)
endif()
target_compile_definitions(RevenantTransformUninline PRIVATE REVENANT_BUILD_PLUGIN)
set_target_properties(RevenantTransformUninline PROPERTIES
  OUTPUT_NAME "RevenantTransformUninline"
  PREFIX ""
  SUFFIX ".dll"
)
llvm_map_components_to_libnames(LLVM_LIBS
  Core
  Support
  Analysis
)
target_link_libraries(RevenantTransformUninline PRIVATE ${LLVM_LIBS})
target_include_directories(RevenantTransformUninline PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
)
set_property(TARGET RevenantTransformUninline PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUninline PROPERTY CXX_STANDARD_REQUIRED ON)
