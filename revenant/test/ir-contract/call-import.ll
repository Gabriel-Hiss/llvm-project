; call to an imported function; import is a declare with its real name and
; optional IAT slot VA
source_filename = "import.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

@g_140005000 = global i32 0, !revenant.va !4
@str_140005100 = constant [7 x i8] c"hello\0a\00", !revenant.va !5

declare i32 @GetLastError() "revenant.va"="0x140009010"

define i32 @sub_140001500() #0 {
bb_140001500:
  %v = load i32, ptr @g_140005000, align 4, !revenant.va !6
  %s = getelementptr inbounds [7 x i8], ptr @str_140005100, i64 0, i64 0, !revenant.va !7
  %r = call i32 @GetLastError(), !revenant.va !8
  %sum = add i32 %v, %r, !revenant.va !9
  ret i32 %sum, !revenant.va !10
}

attributes #0 = { "revenant.va"="0x140001500" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140005000}
!5 = !{i64 u0x140005100}
!6 = !{i64 u0x140001504}
!7 = !{i64 u0x14000150b}
!8 = !{i64 u0x140001512}
!9 = !{i64 u0x140001517}
!10 = !{i64 u0x14000151a}
