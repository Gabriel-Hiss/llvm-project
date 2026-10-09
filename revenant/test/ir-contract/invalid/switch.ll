; BROKEN: "switch" descriptor on a plain br instead of the switch terminator
source_filename = "switch.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @sub_140001400(i32 %sel) #0 {
bb_140001400:
  switch i32 %sel, label %bb_140001430 [
    i32 0, label %bb_140001410
    i32 1, label %bb_140001418
    i32 2, label %bb_140001420
  ], !revenant.va !4
bb_140001410:
  br label %bb_140001440, !revenant.va !6, !revenant.region !5
bb_140001418:
  br label %bb_140001440, !revenant.va !7
bb_140001420:
  br label %bb_140001440, !revenant.va !8
bb_140001430:
  br label %bb_140001440, !revenant.va !9
bb_140001440:
  %r = phi i32 [ 10, %bb_140001410 ], [ 20, %bb_140001418 ],
              [ 30, %bb_140001420 ], [ 0, %bb_140001430 ], !revenant.va !10
  ret i32 %r, !revenant.va !11
}

attributes #0 = { "revenant.va"="0x140001400" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001404}
!5 = !{!"switch", i32 5}
!6 = !{i64 u0x140001410}
!7 = !{i64 u0x140001418}
!8 = !{i64 u0x140001420}
!9 = !{i64 u0x140001430}
!10 = !{i64 u0x140001440}
!11 = !{i64 u0x140001442}
