revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  JumpTableTest.cpp
  )

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantTargetX86
  RevenantDisasm)
