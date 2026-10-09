revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  PaddingTest.cpp
  NoreturnTest.cpp
  XrefsTest.cpp
  StringsTest.cpp
  ResolverTest.cpp
  JumpTableTest.cpp
  CandidatesTest.cpp
  OverlapsTest.cpp
)

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantTargetX86
  RevenantDisasm)

target_compile_definitions(RevenantAnalysisTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe")
