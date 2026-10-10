; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s
; Evidence veto lift: the 2-instruction gep+load callee is below the
; 3-instruction minimum, so the bare shape never outlines. With agreeing
; string evidence on both callee and region, the evidence hold lifts and
; the exact structural match rebuilds the call. Without the metadata the
; same region stays put, so --meta=none output is unchanged.
; CHECK-LABEL: define internal i32 @ev_tiny(
define internal i32 @ev_tiny(ptr %p, i32 %i) !rev.strings !1 {
  %g = getelementptr i32, ptr %p, i32 %i
  %l = load i32, ptr %g, align 4
  ret i32 %l
}

@str_assert = constant [17 x i8] c"assertion failed\00"

; CHECK-LABEL: define i32 @ev_caller(
; CHECK: %[[V:[0-9]+]] = call i32 @ev_tiny(ptr %q, i32 %j)
; CHECK: store i32 %[[V]], ptr %bc, align 4
define i32 @ev_caller(ptr %q, i32 %j) {
entry:
  %g.i = getelementptr i32, ptr %q, i32 %j
  %l.i = load i32, ptr %g.i, align 4
  %bc = bitcast ptr @str_assert to ptr
  store i32 %l.i, ptr %bc, align 4
  ret i32 %l.i
}

!1 = !{!"assertion failed"}
