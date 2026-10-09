# RevenantClangType — lib/Type build
#
# Builds RealTypeBuilder (TypeBuilder.cpp) against the prebuilt
# build-revenant tree plus the Swe2ClangCore interface headers. The
# interface headers live in the Swe2ClangCore worktree until the
# integrator merges; pass -DREVENANT_CLANG_INCLUDE_DIR to point at them.
cmake_minimum_required(VERSION 3.20)
project(RevenantClangType CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED Clang_DIR)
  set(CLANG_CMAKE_DIR "${Clang_DIR}")
endif()
find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_DIR}" NO_DEFAULT_PATH)
find_package(Clang REQUIRED CONFIG PATHS "${CLANG_CMAKE_DIR}" NO_DEFAULT_PATH)

if(NOT DEFINED REVENANT_CLANG_INCLUDE_DIR)
  set(REVENANT_CLANG_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()

llvm_map_components_to_libnames(LLVM_LIBS Demangle DebugInfoDWARF IRReader
                                AsmParser Core Support)

add_library(RevenantClangType STATIC TypeBuilder.cpp)
target_include_directories(RevenantClangType PUBLIC
                           ${REVENANT_CLANG_INCLUDE_DIR})
target_include_directories(RevenantClangType PRIVATE
                           ${LLVM_INCLUDE_DIRS}
                           ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(RevenantClangType PRIVATE ${LLVM_DEFS_LIST})
target_link_libraries(RevenantClangType PUBLIC clangAST clangBasic
                                               clangLex ${LLVM_LIBS})
