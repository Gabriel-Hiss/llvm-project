; BROKEN: unknown region kind "fore"
source_filename = "for.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001300(i32 %n) #0 {
bb_140001300:
  br label %bb_140001308, !revenant.va !4
bb_140001308:
  %i = phi i32 [ 0, %bb_140001300 ], [ %inc, %bb_140001320 ], !revenant.va !5
  %sum = phi i32 [ 0, %bb_140001300 ], [ %sum2, %bb_140001320 ], !revenant.va !6
  %c = icmp slt i32 %i, %n, !revenant.va !7
  br i1 %c, label %bb_140001310, label %bb_140001330, !revenant.va !8, !revenant.region !9
bb_140001310:
  %sum2 = add i32 %sum, %i, !revenant.va !10
  br label %bb_140001320, !revenant.va !11
bb_140001320:
  %inc = add i32 %i, 1, !revenant.va !12
  br label %bb_140001308, !revenant.va !13
bb_140001330:
  ret i32 %sum, !revenant.va !14
}

attributes #0 = { "revenant.va"="0x140001300" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001300}
!5 = !{i64 u0x140001308}
!6 = !{i64 u0x14000130a}
!7 = !{i64 u0x14000130c}
!8 = !{i64 u0x14000130f}
!9 = !{!"fore", i32 1, i32 3, i32 4}
!10 = !{i64 u0x140001310}
!11 = !{i64 u0x140001314}
!12 = !{i64 u0x140001320}
!13 = !{i64 u0x140001324}
!14 = !{i64 u0x140001330}
