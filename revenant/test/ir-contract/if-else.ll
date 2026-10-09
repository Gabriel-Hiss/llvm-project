; if/else region: conditional br annotated with follow block index
source_filename = "branch.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001100(i32 %x) #0 {
bb_140001100:
  %cmp = icmp sgt i32 %x, 0, !revenant.va !4
  br i1 %cmp, label %bb_140001108, label %bb_140001110, !revenant.va !5, !revenant.region !6
bb_140001108:
  %t = add i32 %x, 1, !revenant.va !7
  br label %bb_140001120, !revenant.va !8
bb_140001110:
  %e = sub i32 %x, 1, !revenant.va !9
  br label %bb_140001120, !revenant.va !10
bb_140001120:
  %r = phi i32 [ %t, %bb_140001108 ], [ %e, %bb_140001110 ], !revenant.va !11
  ret i32 %r, !revenant.va !12
}

attributes #0 = { "revenant.va"="0x140001100" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001104}
!5 = !{i64 u0x140001106}
!6 = !{!"if", i32 3}
!7 = !{i64 u0x140001108}
!8 = !{i64 u0x14000110c}
!9 = !{i64 u0x140001110}
!10 = !{i64 u0x140001114}
!11 = !{i64 u0x140001120}
!12 = !{i64 u0x140001122}
