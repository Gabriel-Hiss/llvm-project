# Standalone Revenant subproject: builds against the shared prebuilt
# build-revenant tree (Release + assertions, X86 only). Configure from a
# vcvars64 shell:
#   cmake -G Ninja -S <worktree>/revenant -B <worktree>/build-rv \
#     -DLLVM_DIR=<llvm-project>/build-revenant/lib/cmake/llvm \
#     -DClang_DIR=<llvm-project>/build-revenant/lib/cmake/clang \
#     -DCMAKE_C_COMPILER=<VS clang-cl> -DCMAKE_CXX_COMPILER=<VS clang-cl> \
#     -DCMAKE_C_COMPILER_LAUNCHER=C:/env/sccache.exe \
#     -DCMAKE_CXX_COMPILER_LAUNCHER=C:/env/sccache.exe -DLLVM_USE_LINKER=lld
cmake_minimum_required(VERSION 3.20.0)

project(Revenant CXX C)

# The LLVM 22 headers require C++17; llvm/CMakeLists.txt sets this for
# in-tree builds but nothing installs it for standalone ones.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS NO)

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}: ${LLVM_DIR}")
# Clang is only needed by frontend/clang and slices that link it; it may
# not be in the build tree yet, so keep it optional.
find_package(Clang CONFIG QUIET)
if(Clang_FOUND)
  message(STATUS "Found Clang: ${Clang_DIR}")
else()
  message(STATUS "Clang not found; clang-dependent targets are skipped")
endif()

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(AddLLVM)
include(HandleLLVMOptions)
include(Revenant)

# Match the LLVM build's ABI flags (no EH, no RTTI unless enabled there).
if(NOT LLVM_ENABLE_RTTI)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXXFLAGS_RTTI_DISABLE}")
endif()
if(NOT LLVM_ENABLE_EH)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXXFLAGS_EH_DISABLE}")
endif()

include_directories(${LLVM_INCLUDE_DIRS})
if(Clang_FOUND)
  include_directories(${CLANG_INCLUDE_DIRS})
endif()
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)


# Match the prebuilt Release LLVM libraries' MSVC iterator ABI.
if(WIN32 AND MSVC)
  add_compile_definitions(_ITERATOR_DEBUG_LEVEL=0)
endif()
add_definitions(${LLVM_DEFINITIONS})

# Every slice adds its own CMakeLists under lib/<Area>, tools/<tool>,
# frontend/<tool>, unittests/<Area>. Directories land independently, so
# guard each one.
set(REVENANT_AREA_DIRS
    lib/Core
    lib/Object
    lib/Disasm
    lib/Analysis
    lib/Compiler
    lib/Lift
    lib/Transforms
    lib/Structure
    lib/Export
    lib/CAPI
    lib/IR
    lib/Target/X86
    tools/revenant
    tools/revenant-tblgen
    tools/revenant-ir-verify
    frontend/clang
    unittests)
foreach(dir ${REVENANT_AREA_DIRS})
  if(IS_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/${dir} AND
     EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt)
    add_subdirectory(${dir})
  endif()
endforeach()
