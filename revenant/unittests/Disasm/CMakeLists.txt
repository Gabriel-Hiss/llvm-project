revenant_add_unittest(RevenantUnitTests RevenantDisasmTests
  DisasmTest.cpp
  )

target_link_libraries(RevenantDisasmTests PRIVATE
  RevenantObject
  RevenantDisasm)

target_compile_definitions(RevenantDisasmTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  LLVM_OBJDUMP_EXE="${LLVM_TOOLS_BINARY_DIR}/llvm-objdump.exe")
