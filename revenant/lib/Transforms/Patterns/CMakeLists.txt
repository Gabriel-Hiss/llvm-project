cmake_minimum_required(VERSION 3.20)

# Slice build, two modes (same convention as the Devectorize skeleton):
# - Standalone (this directory is the top-level project): builds the `opt`
#   plugin DLL `RevenantTransformPatterns.dll`, tested with
#   `opt -load-pass-plugin=<dll> -passes=revenant-patterns`. Configure
#   from a vcvars64 shell, e.g.:
#     cmake -G Ninja -S <worktree>/revenant/lib/Transforms/Patterns
#       -B <worktree>/build-rv-patterns
#       -DLLVM_DIR=C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm
#       -DCMAKE_CXX_COMPILER="<VS>/VC/Tools/Llvm/x64/bin/clang-cl.exe"
#       -DLLVM_USE_LINKER=lld -DCMAKE_BUILD_TYPE=Release
#       "-DCMAKE_RC_COMPILER=C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64/rc.exe"
#       "-DCMAKE_MT=<VS>/VC/Tools/Llvm/x64/bin/llvm-mt.exe"
#   then `ninja -j4`.
# - Superbuild (`revenant/` top level, owner: RvSkeleton, via
#   add_subdirectory): builds a static library linked into `revenant-opt`
#   (owner: MuseTxPipeline), which owns the `revenant-patterns` name
#   registration in Pipeline/Register.cpp.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(RevenantPatterns CXX)
  find_package(LLVM REQUIRED CONFIG
    PATHS "C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib/cmake/llvm"
    NO_DEFAULT_PATH)
  message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
  include_directories(${LLVM_INCLUDE_DIRS})
  separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND ${LLVM_DEFINITIONS})
  add_definitions(${LLVM_DEFINITIONS_LIST})
  # Match the shared build: no exceptions, no RTTI.
  if(MSVC)
    add_compile_options(/GR- /EHs-c-)
  else()
    add_compile_options(-fno-exceptions -fno-rtti)
  endif()
endif()

set(PATTERNS_SOURCES Patterns.cpp)

if(COMMAND add_llvm_library)
  add_llvm_library(RevenantTransformPatterns STATIC ${PATTERNS_SOURCES}
    LINK_LIBS LLVMCore LLVMSupport LLVMAnalysis LLVMDemangle
    )
else()
  add_library(RevenantTransformPatterns MODULE ${PATTERNS_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/Patterns.def)
  set_target_properties(RevenantTransformPatterns PROPERTIES
    OUTPUT_NAME "RevenantTransformPatterns"
    PREFIX ""
    SUFFIX ".dll"
    )
  # Link the build-tree .lib files explicitly: llvm_map_components resolves
  # against the install prefix and misses most libraries here.
  set(LLVM_BUILD_LIBDIR
    "C:/Users/User/RustroverProjects/llvm-project/build-revenant/lib")
  target_link_libraries(RevenantTransformPatterns PRIVATE
    ${LLVM_BUILD_LIBDIR}/LLVMCore.lib
    ${LLVM_BUILD_LIBDIR}/LLVMSupport.lib
    ${LLVM_BUILD_LIBDIR}/LLVMAnalysis.lib
    ${LLVM_BUILD_LIBDIR}/LLVMDemangle.lib
    ${LLVM_BUILD_LIBDIR}/LLVMIRReader.lib
    ${LLVM_BUILD_LIBDIR}/LLVMBitReader.lib
    ${LLVM_BUILD_LIBDIR}/LLVMAsmParser.lib
    ${LLVM_BUILD_LIBDIR}/LLVMBinaryFormat.lib
    ${LLVM_BUILD_LIBDIR}/LLVMRemarks.lib
    ${LLVM_BUILD_LIBDIR}/LLVMBitstreamReader.lib
    ${LLVM_BUILD_LIBDIR}/LLVMTargetParser.lib
    ntdll.lib
    )
  target_compile_definitions(RevenantTransformPatterns PRIVATE REVENANT_BUILD_PLUGIN)
  target_include_directories(RevenantTransformPatterns PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../include
    )
endif()
set_property(TARGET RevenantTransformPatterns PROPERTY CXX_STANDARD 17)
set_property(TARGET RevenantTransformPatterns PROPERTY CXX_STANDARD_REQUIRED ON)
