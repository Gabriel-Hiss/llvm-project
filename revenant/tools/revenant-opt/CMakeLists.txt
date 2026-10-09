# revenant-opt: opt with every Revenant pass linked in statically.
#
# Links LLVMOptDriver (opt's own driver library, exported by the shared
# build-revenant tree) plus RevenantTransformPipeline (which in turn links
# every landed RevenantTransform* pass library). Same flags and -passes
# syntax as opt; extra flags -revenant-pipeline / -revenant-patterns run
# the default decompiler pipeline.
#
# NOTE: no AllTargets* pseudo-components. The shared build was configured
# with LLVM_TARGETS_TO_BUILD=X86, and standalone consumers evaluate
# LLVM-Config.cmake before target registration completes, so target
# specifiers are fatal there. List the X86 libs explicitly.
set(LLVM_LINK_COMPONENTS
  X86AsmParser
  X86CodeGen
  X86Desc
  X86Info
  AggressiveInstCombine
  Analysis
  AsmParser
  AsmPrinter
  BitReader
  BitWriter
  CGData
  CFGuard
  CodeGen
  CodeGenTypes
  Core
  Coroutines
  Extensions
  GlobalISel
  IPO
  IRReader
  IRPrinter
  InstCombine
  Instrumentation
  Linker
  MC
  MCDisassembler
  MCParser
  ObjCARCOpts
  Object
  ObjectYAML
  BinaryFormat
  BitstreamReader
  DebugInfoCodeView
  DebugInfoDWARF
  DebugInfoDWARFLowLevel
  DebugInfoMSF
  DebugInfoPDB
  Demangle
  Plugins
  ProfileData
  Remarks
  ScalarOpts
  SandboxIR
  SelectionDAG
  Support
  Symbolize
  Target
  TargetParser
  TextAPI
  TransformUtils
  Vectorize
  FrontendAtomic
  FrontendHLSL
  FrontendOffloading
  FrontendOpenMP
  HipStdPar
  )

add_llvm_tool(revenant-opt
  revenant-opt.cpp
  )

target_link_libraries(revenant-opt
  PRIVATE
  LLVMOptDriver
  RevenantTransformPipeline
  ntdll
  )
