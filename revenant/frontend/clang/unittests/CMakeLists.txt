# RevenantClangTypeTests — gtest for lib/Type. Runs both standalone
# (cmake -S revenant/frontend/clang/unittests) and inside the top-level
# revenant/ superbuild.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
cmake_minimum_required(VERSION 3.20)
project(RevenantClangTypeTests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(DEFINED Clang_DIR)
  set(CLANG_CMAKE_DIR "${Clang_DIR}")
endif()
find_package(LLVM REQUIRED CONFIG PATHS "${LLVM_DIR}" NO_DEFAULT_PATH)
find_package(Clang REQUIRED CONFIG PATHS "${CLANG_CMAKE_DIR}" NO_DEFAULT_PATH)
endif()

if(NOT DEFINED REVENANT_CLANG_INCLUDE_DIR)
  get_filename_component(REVENANT_CLANG_INCLUDE_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/../include" ABSOLUTE)
endif()
if(NOT DEFINED REVENANT_TYPE_SRC_DIR)
  get_filename_component(REVENANT_TYPE_SRC_DIR
      "${CMAKE_CURRENT_SOURCE_DIR}/../lib/Type" ABSOLUTE)
endif()

llvm_map_components_to_libnames(LLVM_LIBS Demangle DebugInfoDWARF
                                DebugInfoCodeView IRReader AsmParser Core
                                Support Option FrontendOpenMP TargetParser)

add_executable(RevenantClangTypeTests
               TypeBuilderTest.cpp
               FrontendContextStub.cpp
               "${REVENANT_TYPE_SRC_DIR}/TypeBuilder.cpp")
target_include_directories(RevenantClangTypeTests PRIVATE
                           ${REVENANT_CLANG_INCLUDE_DIR}
                           ${REVENANT_TYPE_SRC_DIR}
                           ${LLVM_INCLUDE_DIRS}
                           ${CLANG_INCLUDE_DIRS})
separate_arguments(LLVM_DEFS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
target_compile_definitions(RevenantClangTypeTests PRIVATE ${LLVM_DEFS_LIST})
target_link_libraries(RevenantClangTypeTests PRIVATE
                      clangAST clangBasic clangFrontend clangLex clangParse
                      clangSema
                      ${LLVM_LIBS} llvm_gtest llvm_gtest_main)
if(MSVC)
  # LLVM's interface options carry GCC-only -Wno-* flags that cl rejects
  # (they arrive via the clang/LLVM imported targets). Strip them and keep
  # the target at /W0: decompiler output must build warning-free anyway.
  set_target_properties(RevenantClangTypeTests PROPERTIES COMPILE_OPTIONS "")
  target_compile_options(RevenantClangTypeTests PRIVATE /W0)
endif()

add_executable(TypeSmoke TypeSmoke.cpp FrontendContextStub.cpp
               "${REVENANT_TYPE_SRC_DIR}/TypeBuilder.cpp")
target_include_directories(TypeSmoke PRIVATE
                           ${REVENANT_CLANG_INCLUDE_DIR}
                           ${REVENANT_TYPE_SRC_DIR}
                           ${LLVM_INCLUDE_DIRS}
                           ${CLANG_INCLUDE_DIRS})
target_compile_definitions(TypeSmoke PRIVATE ${LLVM_DEFS_LIST})
target_link_libraries(TypeSmoke PRIVATE
                      clangAST clangBasic clangFrontend clangLex clangParse
                      clangSema
                      ${LLVM_LIBS})
if(MSVC)
  set_target_properties(TypeSmoke PROPERTIES COMPILE_OPTIONS "")
  target_compile_options(TypeSmoke PRIVATE /W0)
endif()
