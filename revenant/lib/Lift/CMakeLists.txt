add_library(RevenantLift
  LiftContext.cpp
  X86/X86LiftContext.cpp
  RevenantLift.cpp)
add_dependencies(RevenantLift revenant-lift-gen)
target_include_directories(RevenantLift PRIVATE
  "${CMAKE_BINARY_DIR}/tools/revenant-tblgen"
  "${REVENANT_X86_SRC_DIR}"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
  "${REVENANT_X86_SRC_DIR}/MCTargetDesc")

add_executable(revenant-lift-context-tests LiftContextTests.cpp)
target_link_libraries(revenant-lift-context-tests PRIVATE RevenantLift)
add_custom_target(revenant-lift-test
  COMMAND $<TARGET_FILE:revenant-lift-context-tests>
  DEPENDS revenant-lift-context-tests
  VERBATIM)
target_link_libraries(RevenantLift PUBLIC LLVMCore LLVMMC LLVMSupport)

add_custom_target(revenant-lift-compile DEPENDS RevenantLift)
