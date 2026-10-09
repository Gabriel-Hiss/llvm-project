revenant_add_unittest(RevenantUnitTests RevenantLiftTests
  LiftEHTest.cpp
)

target_link_libraries(RevenantLiftTests PRIVATE
  RevenantLift
  RevenantAnalysis
  RevenantIR)

target_include_directories(RevenantLiftTests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/Function"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include"
)
