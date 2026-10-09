# Revenant standalone build against the prebuilt LLVM/Clang tree.
cmake_minimum_required(VERSION 3.20)
project(Revenant C CXX)

find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM OPTIONAL RESULT_VARIABLE HAVE_ADD_LLVM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_BINARY_DIR}/include
  ${LLVM_INCLUDE_DIRS}
  ${CLANG_INCLUDE_DIRS})

add_definitions(${LLVM_DEFINITIONS})

# Target backend generated headers (X86Gen*.inc) live in the build tree.
if(EXISTS "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86/X86GenInstrInfo.inc")
  set(REVENANT_X86_GEN_DIR "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86")
  set(REVENANT_X86_SRC_DIR "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86")
endif()

# Tools first: revenant-tblgen generates code consumed by lib/Lift.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/revenant-tblgen/CMakeLists.txt")
  add_subdirectory(tools/revenant-tblgen)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lib/Lift/CMakeLists.txt")
  add_subdirectory(lib/Lift)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/unittests/CMakeLists.txt")
  add_subdirectory(unittests)
endif()
