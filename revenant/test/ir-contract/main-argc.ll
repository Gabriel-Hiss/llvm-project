; A libdb-named `main` may lift with fewer arguments than the canonical
; (int, char**) signature — signature recovery proves only argc live-in.
; functionDecl normalizes the FunctionDecl type to (int, char**) regardless,
; so buildFunction must pad ParmVarDecls to the declared arity or
; FunctionDecl::setParams aborts with "Parameter count mismatch!"
; (Decl.cpp:3834) during whole-module --emit source.
source_filename = "main-argc.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @main(i32 %argc) #0 {
bb_140001000:
  %r = add i32 %argc, 1, !revenant.va !4
  ret i32 %r, !revenant.va !5
}

attributes #0 = { "revenant.va"="0x140001000" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001004}
!5 = !{i64 u0x140001008}
