cmake_minimum_required(VERSION 3.20)

# Slice build, two modes:
# - Standalone (standalone project or plain find_package(LLVM) consumer):
#   builds the `opt` plugin DLL `RevenantTransformUnrotate.dll`, tested
#   with `opt -load-pass-plugin=<dll> -passes=revenant-unrotate` and
#   `-passes=revenant-untailrec`.
# - Superbuild (in-tree via add_subdirectory, `add_llvm_library`
#   available): builds a STATIC library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-unrotate` and
#   `revenant-untailrec` name registration in Pipeline/Register.cpp.
#   A MODULE/DLL must never reach the superbuild link: lld-link takes the
#   .dll as an input and the link fails.
set(UNROTATE_SOURCES Unrotate.cpp UntailRec.cpp UnrotatePlugin.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformUnrotate STATIC ${UNROTATE_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis
    )
else()
  add_library(RevenantTransformUnrotate MODULE
    ${UNROTATE_SOURCES}
    UnrotatePlugin.cpp
    )
  set_target_properties(RevenantTransformUnrotate PROPERTIES
    OUTPUT_NAME "RevenantTransformUnrotate"
    PREFIX ""
    SUFFIX ".dll"
    )
  llvm_map_components_to_libnames(LLVM_LIBS Analysis Core Passes Support)
  target_link_libraries(RevenantTransformUnrotate PRIVATE ${LLVM_LIBS})
  target_include_directories(RevenantTransformUnrotate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
  if(TARGET intrinsics_gen)
    add_dependencies(RevenantTransformUnrotate intrinsics_gen)
  endif()
endif()
set_property(TARGET RevenantTransformUnrotate PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformUnrotate PROPERTY CXX_STANDARD_REQUIRED ON)
