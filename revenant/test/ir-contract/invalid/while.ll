; BROKEN: loop header has no loop-kind !revenant.region descriptor
source_filename = "loop.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001200(i32 %n) #0 {
bb_140001200:
  br label %bb_140001210, !revenant.va !4
bb_140001210:
  %i = phi i32 [ 0, %bb_140001200 ], [ %i2, %bb_140001220 ], !revenant.va !5
  %c = icmp ult i32 %i, %n, !revenant.va !6
  br i1 %c, label %bb_140001220, label %bb_140001240, !revenant.va !7
bb_140001220:
  %i2 = add i32 %i, 1, !revenant.va !9
  br label %bb_140001210, !revenant.va !10
bb_140001240:
  ret i32 %i, !revenant.va !11
}

attributes #0 = { "revenant.va"="0x140001200" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001200}
!5 = !{i64 u0x140001210}
!6 = !{i64 u0x140001212}
!7 = !{i64 u0x140001216}
!9 = !{i64 u0x140001220}
!10 = !{i64 u0x140001224}
!11 = !{i64 u0x140001240}
