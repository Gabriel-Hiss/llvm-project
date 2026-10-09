revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  NoreturnTest.cpp
  PaddingTest.cpp
  XrefsTest.cpp
  StringsTest.cpp
  ResolverTest.cpp
  )

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantDisasm)

target_compile_definitions(RevenantAnalysisTests PRIVATE
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe")
