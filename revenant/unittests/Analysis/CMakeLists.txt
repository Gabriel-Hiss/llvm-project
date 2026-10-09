revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  NoreturnTest.cpp
  PaddingTest.cpp
  )

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantDisasm)
