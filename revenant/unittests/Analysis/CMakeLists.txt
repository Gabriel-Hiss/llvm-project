revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  PaddingTest.cpp
  NoreturnTest.cpp
  SignaturesTest.cpp
)

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantDisasm)
