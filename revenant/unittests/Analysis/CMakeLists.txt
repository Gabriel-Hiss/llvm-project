revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  AnalysisTest.cpp
  CandidatesTest.cpp
  JumpTableTest.cpp
  NoreturnTest.cpp
  OrphanTest.cpp
  OverlapsTest.cpp
  PaddingTest.cpp
  ResolverTest.cpp
  SignaturesTest.cpp
  StackPtrTest.cpp
  StringsTest.cpp
  XrefsTest.cpp
  EHTablesTest.cpp
  EmbeddedMetaTest.cpp
  PARTIAL_SOURCES_INTENDED)

revenant_add_unittest(RevenantUnitTests RevenantClassifyTests
  ClassifyTest.cpp
  PARTIAL_SOURCES_INTENDED)

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantTargetX86
  RevenantDisasm)

target_link_libraries(RevenantClassifyTests PRIVATE
  RevenantCAPI
  RevenantAnalysis
  RevenantObject
  RevenantTargetX86
  RevenantDisasm)

target_compile_definitions(RevenantClassifyTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_CPP_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-o2/cpp-o2.exe")
target_compile_definitions(RevenantAnalysisTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_CPP_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-o2/cpp-o2.exe"
  REVENANT_TEST_CPP_CLANG_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-clang-o2/artifacts/cpp-clang-o2.exe"
  REVENANT_TEST_CPP_GCC_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-gcc-o2/cpp-gcc-o2.exe"
  REVENANT_TEST_RUST_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/rust-o2/corpus_rust.exe"
  REVENANT_TEST_JSON_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/json-cl-o2/json-cl-o2.exe"
  REVENANT_TEST_SPDLOG_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/spdlog-cl-o2/spdlog-cl-o2.exe"
)
