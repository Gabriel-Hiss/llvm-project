; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s
; Evidence veto lift: the 2-instruction callee is below the 3-instruction
; minimum, so the bare shape never outlines. With agreeing string evidence
; on both callee and region (the load references the same assert global
; the callee's !rev.strings names), the evidence hold lifts and the exact
; structural match rebuilds the call. Without the metadata the same region
; stays put, so --meta=none output is unchanged.
@msg = constant [17 x i8] c"assertion failed\00"

; CHECK-LABEL: define internal i8 @ev_tiny(
define internal i8 @ev_tiny(ptr %p) !rev.strings !0 {
  %g = getelementptr i8, ptr %p, i64 0
  %e = load i8, ptr getelementptr ([17 x i8], ptr @msg, i32 0, i32 0), align 1
  ret i8 %e
}

; CHECK-LABEL: define i32 @ev_caller(
; CHECK: %[[V:[0-9]+]] = call i8 @ev_tiny(ptr %q)
; CHECK: zext i8 %[[V]] to i32
define i32 @ev_caller(ptr %q) {
entry:
  %g.i = getelementptr i8, ptr %q, i64 0
  %e.i = load i8, ptr getelementptr ([17 x i8], ptr @msg, i32 0, i32 0), align 1
  %z = zext i8 %e.i to i32
  ret i32 %z
}

!0 = !{!"assertion failed"}
