revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  NoreturnTest.cpp
  )

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis)
