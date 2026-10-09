# Revenant Clang frontend: contract .ll -> clang::AST -> C/C++ text.
# Included via add_subdirectory from revenant/CMakeLists.txt. Also builds
# standalone (cmake -S revenant/frontend/clang -B <build>) for slice-local
# work while the top-level tree lands.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20.0)
  project(RevenantClangFrontend CXX C)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  find_package(LLVM REQUIRED CONFIG)
  find_package(Clang REQUIRED CONFIG)
  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  if(EXISTS "${CLANG_CMAKE_DIR}/AddClang.cmake")
    list(APPEND CMAKE_MODULE_PATH "${CLANG_CMAKE_DIR}")
  endif()
  include(AddClang)
  include(AddLLVM)
  include(HandleLLVMOptions)
  if(NOT LLVM_ENABLE_RTTI)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXXFLAGS_RTTI_DISABLE}")
  endif()
  if(NOT LLVM_ENABLE_EH)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LLVM_CXXFLAGS_EH_DISABLE}")
  endif()
  include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  add_definitions(${LLVM_DEFINITIONS})
endif()

include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/include)

set(REVENANT_CLANG_CORE_SOURCES
  lib/Core/Factory.cpp
  lib/Core/FrontendContext.cpp
  lib/Core/GotoStmtBuilder.cpp
  lib/Core/NullTypeBuilder.cpp
  lib/Core/StubExprBuilder.cpp
  lib/Core/TranslationUnit.cpp
  )
# Builder slices land in lib/Stmt, lib/Expr, lib/Type owned by other agents;
# glob them in so a merged tree links them automatically.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/lib/Stmt)
  file(GLOB stmt_srcs CONFIGURE_DEPENDS lib/Stmt/*.cpp)
  list(APPEND REVENANT_CLANG_CORE_SOURCES ${stmt_srcs})
  add_definitions(-DREVENANT_FRONTEND_EXTERNAL_STMT)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/lib/Expr)
  file(GLOB expr_srcs CONFIGURE_DEPENDS lib/Expr/*.cpp)
  list(APPEND REVENANT_CLANG_CORE_SOURCES ${expr_srcs})
  add_definitions(-DREVENANT_FRONTEND_EXTERNAL_EXPR)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/lib/Type)
  file(GLOB type_srcs CONFIGURE_DEPENDS lib/Type/*.cpp)
  list(APPEND REVENANT_CLANG_CORE_SOURCES ${type_srcs})
  add_definitions(-DREVENANT_FRONTEND_EXTERNAL_TYPE)
endif()

add_llvm_library(RevenantClangCore DISABLE_LLVM_LINK_LLVM_DYLIB
  ${REVENANT_CLANG_CORE_SOURCES}

  LINK_COMPONENTS
  AsmParser
  BinaryFormat
  Core
  Support
  )

clang_target_link_libraries(RevenantClangCore PRIVATE
  clangAST
  clangBasic
  )

add_llvm_executable(revenant-clang
  tools/revenant-clang/main.cpp
  )

target_link_libraries(revenant-clang PRIVATE RevenantClangCore)

clang_target_link_libraries(revenant-clang PRIVATE
  clangAST
  clangBasic
  )
