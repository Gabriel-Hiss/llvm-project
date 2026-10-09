set(LLVM_LINK_COMPONENTS Core)
revenant_add_unittest(RevenantUnitTests RevenantIdiomsTests
  IdiomsTest.cpp
  )
set(LLVM_LINK_COMPONENTS)

target_link_libraries(RevenantIdiomsTests PRIVATE RevenantTransformIdioms)
