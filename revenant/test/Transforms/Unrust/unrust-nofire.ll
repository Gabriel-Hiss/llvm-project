; Negative tests: shapes the pass must leave alone.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @panic_bounds_check(i64, i64, ptr)

; A plain signed compare feeding a branch is not a bounds check.
; CHECK-LABEL: define i64 @signed_cmp(
define i64 @signed_cmp(i64 %a, i64 %b) {
entry:
; CHECK: %c = icmp slt i64 %a, %b
  %c = icmp slt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; A plain add without nsw/nuw is not a folded overflow check, even when
; the function panics elsewhere.
; CHECK-LABEL: define i64 @plain_add(
define i64 @plain_add(i64 %a, i64 %b, ptr %slot) {
entry:
; CHECK: %s = add i64 %a, %b
  %s = add i64 %a, %b
  store i64 %s, ptr %slot, align 8
  call void @panic_bounds_check(i64 %a, i64 %b, ptr %slot)
  ret i64 %s
}

; An unsigned compare with no panic anywhere is a generic compare.
; CHECK-LABEL: define i64 @generic_ugt(
define i64 @generic_ugt(i64 %a, i64 %b) {
entry:
; CHECK: %c = icmp ugt i64 %a, %b
  %c = icmp ugt i64 %a, %b
  br i1 %c, label %t, label %f
t:
  ret i64 %a
f:
  ret i64 %b
}

; A samesign range idiom with no panic edge is not a bounds check.
; CHECK-LABEL: define i32 @range_idiom(
define i32 @range_idiom(i32 %c) {
entry:
; CHECK: %t = icmp samesign ult i32 %c, 128
  %t = icmp samesign ult i32 %c, 128
  br i1 %t, label %yes, label %no
yes:
  ret i32 1
no:
  ret i32 0
}
