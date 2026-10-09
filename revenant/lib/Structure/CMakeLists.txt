add_llvm_library(RevenantStructure
  RevenantStructure.cpp

  LINK_COMPONENTS
  Analysis
  Core
  Support
  )

target_include_directories(RevenantStructure PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include)

add_subdirectory(tool)
