revenant_add_unittest(RevenantUnitTests RevenantLiftTests
  LiftTest.cpp
  )

target_link_libraries(RevenantLiftTests PRIVATE
  RevenantLift
  RevenantObject
  RevenantDisasm
  RevenantIR)

target_compile_definitions(RevenantLiftTests PRIVATE
  REVENANT_TEST_C_O0="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o0/c-o0.exe"
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_CPP_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-o2/cpp-o2.exe")

target_include_directories(RevenantLiftTests PRIVATE
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86/MCTargetDesc"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
)
