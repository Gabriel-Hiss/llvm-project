revenant_add_unittest(RevenantUnitTests RevenantLiftTests
  LiftEHTest.cpp
  LiftTest.cpp
  PARTIAL_SOURCES_INTENDED
  )

target_link_libraries(RevenantLiftTests PRIVATE
  RevenantLift
  RevenantAnalysis
  RevenantIR
  RevenantObject
  RevenantDisasm
  RevenantTargetX86)

target_compile_definitions(RevenantLiftTests PRIVATE
  REVENANT_TEST_C_O0="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o0/c-o0.exe"
  REVENANT_TEST_C_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-o2/c-o2.exe"
  REVENANT_TEST_CPP_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/cpp-o2/cpp-o2.exe"
  REVENANT_TEST_C_CLANG_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-clang-o2/c-clang-o2.exe"
  REVENANT_TEST_C_GCC_O2="C:/Users/User/RustroverProjects/Revenant/target/parity/c-gcc-o2/c-gcc-o2.exe")

target_include_directories(RevenantLiftTests PRIVATE
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86/MCTargetDesc"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/Function"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../include")

# Differential test: lifted IR, compiled by the JIT, against native x86-64
# execution of the same bytes (assembled with the build tree's llvm-mc).
set(LLVM_LINK_COMPONENTS OrcJIT native Support)
revenant_add_unittest(RevenantUnitTests RevenantLiftSemanticsTests
  LiftSemanticsTest.cpp
  LiftSemanticsCases.cpp
  PARTIAL_SOURCES_INTENDED
  )
set(LLVM_LINK_COMPONENTS)

target_link_libraries(RevenantLiftSemanticsTests PRIVATE
  RevenantLift
  RevenantDisasm
  RevenantObject
  RevenantIR)

target_compile_definitions(RevenantLiftSemanticsTests PRIVATE
  REVENANT_TEST_LLVM_MC="${LLVM_BUILD_BINARY_DIR}/bin/llvm-mc.exe")

target_include_directories(RevenantLiftSemanticsTests PRIVATE
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86"
  "${LLVM_BUILD_MAIN_SRC_DIR}/lib/Target/X86/MCTargetDesc"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
)
