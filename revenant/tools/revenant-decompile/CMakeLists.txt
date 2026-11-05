add_llvm_tool(revenant-decompile
  revenant-decompile.cpp
  )

target_link_libraries(revenant-decompile PRIVATE
  RevenantCAPI
  )

# Twenty lift and clean workers allocate and free metadata at the same time.
# The default NT heap serializes them on one lock; the segment heap keeps
# per-thread state and scales (Windows 10 2004 and later, ignored before).
if(MSVC)
  target_link_options(revenant-decompile PRIVATE
    "/MANIFEST:EMBED"
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/revenant-decompile.manifest")
endif()
