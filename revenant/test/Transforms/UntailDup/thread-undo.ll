; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-untaildup -S %s | %FileCheck %s
; Jump-threading undo at the clone level: two identical branch blocks over
; the same successor pair fold into one. Pre shape mirrors the
; `simplifycfg<no-hoist-common-insts>` join in c-clang-o2 `nested_if`.

define i32 @t(i1 %a, i1 %b, i32 %x) {
; CHECK-LABEL: define i32 @t(
entry:
  br i1 %a, label %P0, label %P1
P0:
  br i1 %b, label %TT, label %TF
P1:
  br i1 %b, label %TT, label %TF
; CHECK-NOT: P1:
TT:
  %p = add i32 %x, 1, !revenant.va !{i64 4096}
  ret i32 %p
TF:
  %q = add i32 %x, 2, !revenant.va !{i64 4112}
  ret i32 %q
}
