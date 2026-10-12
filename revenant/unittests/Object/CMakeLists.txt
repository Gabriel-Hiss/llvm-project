revenant_add_unittest(RevenantUnitTests RevenantObjectTests
  PeFileTest.cpp
  )

target_link_libraries(RevenantObjectTests PRIVATE RevenantObject RevenantAnalysis RevenantTargetX86)

# Corpus binary the loader is checked against, and the Rust reference
# CLI used as the oracle for section/pdata/import counts.
target_compile_definitions(RevenantObjectTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_AUDIO_CL_O1="C:/Users/User/RustroverProjects/Revenant/target/parity/audio-nodegraph-cl-o1/audio-nodegraph-cl-o1.dll"
  REVENANT_TEST_AUDIO_CLANG_O3="C:/Users/User/RustroverProjects/Revenant/target/parity/audio-nodegraph-clang-o3/audio-nodegraph-clang-o3.dll"
  REVENANT_TEST_C_GCC_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-gcc-o2/c-gcc-o2.exe"
  REVENANT_TEST_JSON_CL_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/json-cl-o2/json-cl-o2.exe"
  REVENANT_TEST_GO_UNOPTIMIZED="C:/Users/User/RustroverProjects/Revenant/target/parity/go-unoptimized/divisiveis.exe"
  REVENANT_TEST_GO_CARDS="C:/Users/User/RustroverProjects/Revenant/target/parity/go-cards-o0/cr-card-counter.exe"
  REVENANT_TEST_BACKUPD="C:/Users/User/RustroverProjects/Revenant/target/parity/backupd-o0/backupD.exe"
  REVENANT_TEST_LUA_GCC="C:/Users/User/RustroverProjects/Revenant/target/parity/lua-gcc-o0/lua-gcc-o0.exe"
  REVENANT_CLI_EXE="C:/Users/User/RustroverProjects/Revenant/target/release/revenant-cli.exe"
  LLVM_OBJDUMP_EXE="${LLVM_TOOLS_BINARY_DIR}/llvm-objdump.exe")
