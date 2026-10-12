revenant_add_unittest(RevenantUnitTests RevenantObjectTests
  PeFileTest.cpp
  )

target_link_libraries(RevenantObjectTests PRIVATE RevenantObject)

# Corpus binary the loader is checked against, and the Rust reference
# CLI used as the oracle for section/pdata/import counts.
target_compile_definitions(RevenantObjectTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_GO_UNOPTIMIZED="C:/Users/User/RustroverProjects/Revenant/target/parity/go-unoptimized/divisiveis.exe"
  REVENANT_CLI_EXE="C:/Users/User/RustroverProjects/Revenant/target/release/revenant-cli.exe"
  LLVM_OBJDUMP_EXE="${LLVM_TOOLS_BINARY_DIR}/llvm-objdump.exe")
