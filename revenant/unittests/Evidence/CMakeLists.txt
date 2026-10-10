set(LLVM_LINK_COMPONENTS Core)
revenant_add_unittest(RevenantUnitTests RevenantEvidenceTests
  EvidenceTest.cpp
  )
set(LLVM_LINK_COMPONENTS)
