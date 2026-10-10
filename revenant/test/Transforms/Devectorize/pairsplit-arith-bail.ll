; Negative: an i128 value with real arithmetic stays untouched. Splitting
; the store would strand the add in i128 and rebuild it uglier, so the web
; bails whole.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

define void @pair_arith_bail(ptr %p, i128 %x) {
entry:
  %v = load i128, ptr %p, align 1
  %s = add i128 %v, %x
  store i128 %s, ptr %p, align 1
  ret void
}

; CHECK: load i128
; CHECK: add i128
; CHECK: store i128
; CHECK: ret void
