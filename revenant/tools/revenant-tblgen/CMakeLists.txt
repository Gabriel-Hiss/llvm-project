add_executable(revenant-tblgen
  revenant-tblgen.cpp
  RevenantLiftEmitter.cpp)
target_include_directories(revenant-tblgen PRIVATE
  "${LLVM_BUILD_MAIN_SRC_DIR}/utils/TableGen"
  "${LLVM_BUILD_MAIN_SRC_DIR}/utils/TableGen/Basic"
  "${LLVM_BUILD_MAIN_SRC_DIR}/utils/TableGen/Common")

target_link_libraries(revenant-tblgen PRIVATE
  LLVMTableGen
  LLVMTableGenCommon
  LLVMTableGenBasic
  LLVMSupport)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/RevenantLift.inc
  COMMAND revenant-tblgen -gen-revenant-lift
          -I ${REVENANT_X86_SRC_DIR}
          -I ${LLVM_BUILD_MAIN_SRC_DIR}/include
          -I ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/RevenantX86.td
          -o ${CMAKE_CURRENT_BINARY_DIR}/RevenantLift.inc
  DEPENDS revenant-tblgen
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/RevenantX86.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/X86Lift.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/X87.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/StringAtomic.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/System.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/Sse.td
          ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Lift/X86/Avx.td
  VERBATIM)

add_custom_target(revenant-lift-gen DEPENDS
  ${CMAKE_CURRENT_BINARY_DIR}/RevenantLift.inc)

