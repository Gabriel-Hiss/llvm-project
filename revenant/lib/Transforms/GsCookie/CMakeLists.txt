if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
cmake_minimum_required(VERSION 3.20)
project(RevenantGsCookie CXX)

# Standalone slice build. Configured directly on this directory from a
# vcvars64 shell:
#   cmake -G Ninja -S <worktree>/revenant/lib/Transforms/GsCookie
#     -B <worktree>/build-rv-gscookie
#     -DLLVM_DIR=C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm
#     -DCMAKE_C_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#     -DCMAKE_CXX_COMPILER="C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#     -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe
#     -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe
#     -DLLVM_USE_LINKER=lld -DCMAKE_BUILD_TYPE=Release
# then `ninja -j4`. The top-level `revenant/` superbuild and
# `revenant-opt` pick this directory up with add_subdirectory; keep this
# file self-sufficient for both uses.
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

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformGsCookie STATIC GsCookie.cpp
    LINK_COMPONENTS Core Support Analysis TransformUtils
    )
else()
  add_library(RevenantTransformGsCookie MODULE GsCookie.cpp)
  target_compile_definitions(RevenantTransformGsCookie PRIVATE
    REVENANT_BUILD_PLUGIN)
  set_target_properties(RevenantTransformGsCookie PROPERTIES
    OUTPUT_NAME "RevenantTransformGsCookie"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS
    Core
    Support
    Analysis
    TransformUtils
    )
  target_link_libraries(RevenantTransformGsCookie PRIVATE ${LLVM_LIBS})
  target_include_directories(RevenantTransformGsCookie PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformGsCookie PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformGsCookie PROPERTY CXX_STANDARD_REQUIRED ON)
