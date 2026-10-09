cmake_minimum_required(VERSION 3.20.0)
project(revenant-unrotate-standalone CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Standalone slice build against the shared build-revenant tree. Only this
# slice compiles here; never reconfigure or rebuild build-revenant itself.
find_package(LLVM REQUIRED CONFIG
  PATHS "C:/Users/User/RustroverProjects/Revenant/llvm-project/build-revenant/lib/cmake/llvm"
  NO_DEFAULT_PATH)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
include_directories(${LLVM_INCLUDE_DIRS})
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
add_definitions(${LLVM_DEFINITIONS_LIST})
include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
add_subdirectory(lib/Transforms/Unrotate)
