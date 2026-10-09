; BROKEN: the cpuid call is missing !revenant.opaque
source_filename = "opaque.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare void @revenant.asm.pause()
declare i64 @revenant.asm.cpuid(i64)

define void @sub_140001900(i64 %seed) #0 {
bb_140001900:
  br label %bb_140001908, !revenant.va !4
bb_140001908:
  %i = phi i64 [ %seed, %bb_140001900 ], [ %i2, %bb_140001908 ], !revenant.va !5
  call void @revenant.asm.pause(), !revenant.va !6, !revenant.opaque !7
  %r = call i64 @revenant.asm.cpuid(i64 %i), !revenant.va !8
  %i2 = xor i64 %i, %r, !revenant.va !9
  %c = icmp eq i64 %i2, 0, !revenant.va !10
  br i1 %c, label %bb_140001930, label %bb_140001908, !revenant.va !11, !revenant.region !12
bb_140001930:
  ret void, !revenant.va !13
}

attributes #0 = { "revenant.va"="0x140001900" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001900}
!5 = !{i64 u0x140001908}
!6 = !{i64 u0x14000190a}
!7 = !{}
!8 = !{i64 u0x14000190e}
!9 = !{i64 u0x140001914}
!10 = !{i64 u0x140001917}
!11 = !{i64 u0x14000191b}
!12 = !{!"dowhile", i32 1, i32 1, i32 2}
!13 = !{i64 u0x140001930}
