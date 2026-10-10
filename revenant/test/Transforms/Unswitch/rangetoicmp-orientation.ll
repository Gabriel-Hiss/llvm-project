; RUN: %revenant-opt -passes=revenant-unswitch -S %s | %FileCheck %s

; Range bound on the false edge (`cmp; ja default`) is also a plain
; interval check, so the range-to-switch family must leave the branch
; alone: expanding it into a switch all of whose cases share one
; destination would lose the source-level `if`.
define i32 @ugt_kept(i32 %x) {
entry:
  %c = icmp ugt i32 %x, 7
  br i1 %c, label %big, label %small

small:
  %s = add i32 %x, 1
  br label %join

big:
  %b = sub i32 %x, 1
  br label %join

join:
  %p = phi i32 [ %s, %small ], [ %b, %big ]
  ret i32 %p
}

; CHECK-LABEL: define i32 @ugt_kept(
; CHECK:       %c = icmp ugt i32 %x, 7
; CHECK:       br i1 %c, label %big, label %small
; CHECK-NOT:   switch

; `cmp; jae` (ule) is the same shape from the other side.
define i32 @ule_kept(i32 %x) {
entry:
  %c = icmp ule i32 %x, 7
  br i1 %c, label %small, label %big

small:
  %s = add i32 %x, 1
  br label %join

big:
  %b = sub i32 %x, 1
  br label %join

join:
  %p = phi i32 [ %s, %small ], [ %b, %big ]
  ret i32 %p
}

; CHECK-LABEL: define i32 @ule_kept(
; CHECK:       %c = icmp ule i32 %x, 7
; CHECK:       br i1 %c, label %small, label %big
; CHECK-NOT:   switch

; The in-range-on-true orientation is what the family expands.
define i32 @ult_expands(i32 %x) {
entry:
  %c = icmp ult i32 %x, 8
  br i1 %c, label %small, label %big

small:
  %s = add i32 %x, 1
  br label %join

big:
  %b = sub i32 %x, 1
  br label %join

join:
  %p = phi i32 [ %s, %small ], [ %b, %big ]
  ret i32 %p
}

; CHECK-LABEL: define i32 @ult_expands(
; CHECK:       switch i32 %x, label %big [
; CHECK:         i32 0, label %small
; CHECK:         i32 7, label %small
; CHECK:       ]
