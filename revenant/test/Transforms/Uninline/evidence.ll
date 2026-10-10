; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s
; Evidence-confirmed tiny bodies: a 2-instruction callee is below the
; structural minimum, so it outlines only when lift-recorded evidence
; agrees with the candidate region. The callee below reads its assert
; string (the assert-string family); the confirming caller's inlined copy
; references the same string global. A same-shaped copy referencing a
; different string must survive untouched. (Single-block evidence bodies
; are call-free by construction, so confirmation here is string-based.)
; CHECK-LABEL: define internal i8 @ev_firstbyte(
define internal i8 @ev_firstbyte() !rev.strings !0 {
  %p = getelementptr inbounds [17 x i8], ptr @str_assert, i64 0, i64 0
  %v = load i8, ptr %p, align 1
  ret i8 %v
}

@str_assert = internal constant [17 x i8] c"assertion failed\00"
@str_other = internal constant [12 x i8] c"all is well\00"

; CHECK-LABEL: define i8 @ev_confirmed(
; CHECK: call i8 @ev_firstbyte()
define i8 @ev_confirmed() {
entry:
  %p.i = getelementptr inbounds [17 x i8], ptr @str_assert, i64 0, i64 0
  %v.i = load i8, ptr %p.i, align 1
  ret i8 %v.i
}

; The same shape over a different string must survive untouched.
; CHECK-LABEL: define i8 @ev_unconfirmed(
; CHECK: getelementptr
; CHECK: load i8
; CHECK-NOT: call i8 @ev_firstbyte
define i8 @ev_unconfirmed() {
entry:
  %p.i = getelementptr inbounds [12 x i8], ptr @str_other, i64 0, i64 0
  %v.i = load i8, ptr %p.i, align 1
  ret i8 %v.i
}

!0 = !{!"assertion failed"}
