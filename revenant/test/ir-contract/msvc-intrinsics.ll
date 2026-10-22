; MSVC intrinsics: the lifter spells `llvm.x86.int 0x29` as the MSVC
; `__fastfail` intrinsic. The declaration only exists in <intrin.h> on the
; MSVC ABI (a plain prototype links LNK2019), so the emitted TU must pull
; the header in (board line 339 family: C3861 __fastfail undeclared).
source_filename = "cfguard.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

declare void @llvm.x86.int(i8)

define void @guard_dispatch(i8 %ok) #0 {
bb_140001000:
  %c = icmp eq i8 %ok, 41, !revenant.va !4
  br i1 %c, label %bb_140001010, label %bb_140001020, !revenant.va !5
bb_140001010:
  call void @llvm.x86.int(i8 41), !revenant.va !6
  ret void, !revenant.va !7
bb_140001020:
  ret void, !revenant.va !8
}

attributes #0 = { "revenant.va"="0" }

!llvm.module.flags = !{!0}

!0 = !{i32 4, !"revenant.lang", !"c"}
!4 = !{i64 u0x140001000}
!5 = !{i64 u0x140001002}
!6 = !{i64 u0x140001008}
!7 = !{i64 u0x14000100a}
!8 = !{i64 u0x14000100c}
