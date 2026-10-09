add_library(RevenantLiftGenerated
  LiftContext.cpp
  X86/X86LiftContext.cpp
  X86/X86LiftIntrinsics.cpp
  X86/X86LiftAvx512.cpp
  X86/X86LiftVector.cpp
  RevenantLift.cpp)
add_dependencies(RevenantLiftGenerated revenant-lift-gen)
target_include_directories(RevenantLiftGenerated PRIVATE
  "${CMAKE_BINARY_DIR}/tools/revenant-tblgen"
  "${REVENANT_X86_SRC_DIR}"
  "${LLVM_BUILD_BINARY_DIR}/lib/Target/X86"
  "${REVENANT_X86_SRC_DIR}/MCTargetDesc")
target_link_libraries(RevenantLiftGenerated PUBLIC LLVMCore LLVMMC LLVMSupport)

add_executable(revenant-lift-context-tests LiftContextTests.cpp)
target_link_libraries(revenant-lift-context-tests PRIVATE RevenantLiftGenerated)
add_custom_target(revenant-lift-test
  COMMAND $<TARGET_FILE:revenant-lift-context-tests>
  DEPENDS revenant-lift-context-tests
  VERBATIM)
add_custom_target(revenant-lift-compile DEPENDS RevenantLiftGenerated)

add_subdirectory(Function)
target_link_libraries(RevenantLift PUBLIC RevenantLiftGenerated)
