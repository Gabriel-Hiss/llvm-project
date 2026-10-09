add_llvm_tool(revenant
  revenant.cpp
  ExportParity.cpp
  )

target_link_libraries(revenant PRIVATE
  RevenantObject
  RevenantDisasm
  RevenantAnalysis
  RevenantTargetX86
  )
# compare.py drives "<exporter> <binary> <outdir>"; the .cmd adapts that
# to "revenant --export-parity <binary> <outdir>" and must sit next to
# the exe it launches.
add_custom_command(TARGET revenant POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${CMAKE_CURRENT_SOURCE_DIR}/export-parity.cmd
    $<TARGET_FILE_DIR:revenant>/export-parity.cmd)
