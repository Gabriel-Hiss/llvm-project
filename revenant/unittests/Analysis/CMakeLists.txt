revenant_add_unittest(RevenantUnitTests RevenantAnalysisTests
  StackPtrTest.cpp
  )

target_link_libraries(RevenantAnalysisTests PRIVATE
  RevenantAnalysis
  RevenantObject
  RevenantDisasm
  RevenantTargetX86)
