revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  PaddingTest.cpp
  NoreturnTest.cpp
)

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantDisasm)
