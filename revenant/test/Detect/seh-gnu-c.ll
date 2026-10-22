; Board line 347: MinGW CRT startup lifts to invoke/catchswitch/catchpad with
; the __C_specific_handler personality. On an x86_64-w64-windows-gnu triple in
; a C module neither __try/__except (MSVC extension) nor try/catch (C++) is
; valid C, so the emitter must drop the handler wrapper and keep only the
; protected body. Requires revenant-clang (built in the worktree).
; RUN: %revenant-clang %s --lang=c | %FileCheck %s
source_filename = "crt.exe"
target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-w64-windows-gnu"

declare i32 @__scrt_acquire_startup_lock() "revenant.va"="0x140009020"
declare i32 @__C_specific_handler() "revenant.va"="0x140009030"

define i32 @sub_1400010F6() #0 personality ptr @__C_specific_handler {
bb_1400010F6:
  %r = invoke i32 @__scrt_acquire_startup_lock()
          to label %bb_140001100 unwind label %bb_140001110, !revenant.va !6
bb_140001100:
  br label %bb_140001120, !revenant.va !7
bb_140001110:
  %sw = catchswitch within none [label %bb_140001130] unwind to caller, !revenant.va !8
bb_140001130:
  %pad = catchpad within %sw [ptr null], !revenant.va !9
  catchret from %pad to label %bb_140001120, !revenant.va !10
bb_140001120:
  ret i32 0, !revenant.va !11
}

attributes #0 = { "revenant.va"="0x1400010F6" }

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"c"}
!6 = !{i64 u0x1400010F6}
!7 = !{i64 u0x140001100}
!8 = !{i64 u0x140001110}
!9 = !{i64 u0x140001130}
!10 = !{i64 u0x140001138}
!11 = !{i64 u0x140001120}

; No spelling of the SEH handler survives in C on a gnu triple.
; CHECK-NOT: __try
; CHECK-NOT: __except
; CHECK-NOT: try {
; CHECK-NOT: catch (
; The protected body still runs.
; CHECK: sub_1400010F6
; CHECK: __scrt_acquire_startup_lock()
