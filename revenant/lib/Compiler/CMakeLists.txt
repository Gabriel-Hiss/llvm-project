if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.20.0)
  project(RevenantCompilerProfile C CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  find_package(LLVM REQUIRED CONFIG)
  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  include(AddLLVM)
  include(HandleLLVMOptions)
  include_directories(${LLVM_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
  add_definitions(${LLVM_DEFINITIONS})
endif()

add_library(RevenantCompilerProfile STATIC Profile.cpp)
llvm_map_components_to_libnames(LLVM_LIBS Support)
target_link_libraries(RevenantCompilerProfile PUBLIC ${LLVM_LIBS})
target_include_directories(RevenantCompilerProfile PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
