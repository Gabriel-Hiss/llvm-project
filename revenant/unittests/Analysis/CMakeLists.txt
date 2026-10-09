revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  AnalysisTest.cpp
  CandidatesTest.cpp
  JumpTableTest.cpp
  NoreturnTest.cpp
  OrphanTest.cpp
  OverlapsTest.cpp
  PaddingTest.cpp
  ResolverTest.cpp
  StackPtrTest.cpp
  StringsTest.cpp
  XrefsTest.cpp
  SignaturesTest.cpp
)

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantTargetX86
  RevenantDisasm)

target_compile_definitions(RevenantAnalysisTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe")
