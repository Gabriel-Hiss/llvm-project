; RUN: %revenant-opt -passes=revenant-uninline -S %s | %FileCheck %s
; Store-preserving outline: a straight-line callee with plain stores is
; outlined back into a call at the same stream position, so every address,
; value and relative order is preserved. Volatile stores stay rejected.
; CHECK-LABEL: define internal void @store_init(
define internal void @store_init(ptr %p, i32 %v) {
  %g = getelementptr i32, ptr %p, i64 22
  store i32 %v, ptr %g, align 1
  %h = getelementptr i32, ptr %p, i64 29
  %w = load i32, ptr %h, align 1
  %n = add i32 %w, -1
  store i32 %n, ptr %h, align 1
  ret void
}

; CHECK-LABEL: define void @store_caller(
; CHECK: call void @store_init(ptr %q, i32 %x)
define void @store_caller(ptr %q, i32 %x, ptr %dst) {
entry:
  %g.i = getelementptr i32, ptr %q, i64 22
  store i32 %x, ptr %g.i, align 1
  %h.i = getelementptr i32, ptr %q, i64 29
  %w.i = load i32, ptr %h.i, align 1
  %n.i = add i32 %w.i, -1
  store i32 %n.i, ptr %h.i, align 1
  store i32 %x, ptr %dst, align 4
  ret void
}

; A volatile store in the callee body vetoes the whole callee.
; CHECK-LABEL: define internal void @vol_init(
define internal void @vol_init(ptr %p, i32 %v) {
  %g = getelementptr i32, ptr %p, i64 22
  store volatile i32 %v, ptr %g, align 1
  %h = getelementptr i32, ptr %p, i64 29
  %w = load i32, ptr %h, align 1
  %n = add i32 %w, -1
  store i32 %n, ptr %h, align 1
  ret void
}

; CHECK-LABEL: define void @vol_caller(
; CHECK: store volatile i32
; CHECK-NOT: call void @vol_init
define void @vol_caller(ptr %q, i32 %x, ptr %dst) {
entry:
  %g.i = getelementptr i32, ptr %q, i64 22
  store volatile i32 %x, ptr %g.i, align 1
  %h.i = getelementptr i32, ptr %q, i64 29
  %w.i = load i32, ptr %h.i, align 1
  %n.i = add i32 %w.i, -1
  store i32 %n.i, ptr %h.i, align 1
  store i32 %n.i, ptr %dst, align 4
  ret void
}
