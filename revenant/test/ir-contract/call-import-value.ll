; A value use of a call to a declaration-only import. The callee is read
; through the call's own function type, so a prototype the compiler can
; shadow (system headers, a recovered `void` return) cannot make the use
; ill-formed (MSVC C2296/C2069/C2561) while the IR value keeps its type.
source_filename = "import.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare i64 @imported_exe_i64()

define i64 @sub_140001500(i64 %0) #0 {
bb_140001500:
  %r = call i64 @imported_exe_i64(), !revenant.va !2
  %m = and i64 %r, 61440, !revenant.va !3
  ret i64 %m, !revenant.va !4
}

attributes #0 = { "revenant.va"="0x140001500" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!2 = !{i64 u0x140001502}
!3 = !{i64 u0x140001504}
!4 = !{i64 u0x140001505}
