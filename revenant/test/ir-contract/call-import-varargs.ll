; Varargs import whose result is used: the cast prototype must keep the
; variadic form so extra arguments still type-check at the call.
source_filename = "import.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare i32 @imported_varargs(ptr, ...)

define i32 @sub_140001600(i64 %0, ptr %1) #0 {
bb_140001600:
  %r = call i32 (ptr, ...) @imported_varargs(ptr %1, i64 %0, i64 7), !revenant.va !2
  ret i32 %r, !revenant.va !3
}

attributes #0 = { "revenant.va"="0x140001600" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c" }
!2 = !{i64 u0x140001602}
!3 = !{i64 u0x140001608}
