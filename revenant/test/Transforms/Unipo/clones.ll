; Clone fold-back: GCC `.constprop.N`/`.isra.N` and LLVM
; `.specialized.N`/`.lto_priv` symbols (see gcc-*.map: `_S_construct...isra.0`,
; `pool...constprop.0`) fold back under the base name when no live base
; definition exists in the module.
; RUN: %revenant-opt -passes=revenant-unipo -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define internal i32 @worker(ptr %p)
define internal i32 @worker.constprop.0(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

; Clone with a body call keeps its name (calls veto the rename; the
; return-restore rule would otherwise claim the dead call result).
; CHECK-LABEL: define internal void @helper.isra.0(ptr %p)
define internal void @helper.isra.0(ptr %p) {
entry:
  call void @sink(ptr %p)
  ret void
}

; CHECK-LABEL: define internal i64 @spec(i64 %x)
define internal i64 @spec.specialized.1(i64 %x) {
entry:
  %y = add i64 %x, 1
  ret i64 %y
}

declare void @sink(ptr)

; No-fire: a live base keeps the clone (merging is revenant-uninline's job).
; CHECK-LABEL: define internal i32 @livebase.constprop.0(ptr %p)
define internal i32 @livebase.constprop.0(ptr %p) {
entry:
  %v = load i32, ptr %p, align 4
  ret i32 %v
}

; CHECK-LABEL: define internal i32 @livebase(ptr %p, i32 %q)
define internal i32 @livebase(ptr %p, i32 %q) {
entry:
  ret i32 %q
}
