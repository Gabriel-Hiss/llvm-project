# Lit/test runner for RevenantStructure: .ll -> annotated .ll.
add_llvm_executable(rv-structure-tool
  rv-structure-tool.cpp
  )

target_link_libraries(rv-structure-tool PRIVATE
  RevenantStructure
  )
