add_llvm_tool(revenant
  revenant.cpp
  ExportParity.cpp
  )

target_link_libraries(revenant PRIVATE
  RevenantObject
  RevenantDisasm
  )

# The export runs on RevenantAnalysis once that slice lands; until then
# ExportParity.cpp's fallback keeps the driver buildable (the export
# prints `analysis=pending`). The glob makes the first
# lib/Analysis/CMakeLists.txt to appear retrigger configure so the
# TARGET check flips on its own.
file(GLOB REVENANT_ANALYSIS_CMAKELISTS CONFIGURE_DEPENDS
     ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/Analysis/CMakeLists.txt)
if(TARGET RevenantAnalysis)
  target_link_libraries(revenant PRIVATE RevenantAnalysis RevenantTargetX86)
  target_compile_definitions(revenant PRIVATE REVENANT_HAVE_ANALYSIS)
endif()
# compare.py drives "<exporter> <binary> <outdir>"; the .cmd adapts that
# to "revenant --export-parity <binary> <outdir>" and must sit next to
# the exe it launches.
add_custom_command(TARGET revenant POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/export-parity.cmd
    $<TARGET_FILE_DIR:revenant>/export-parity.cmd)
