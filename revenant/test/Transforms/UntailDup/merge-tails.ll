; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-untaildup -S %s | %FileCheck %s
; Corpus: c-clang-o2 `early_returns` (ir-pre) has four `store+br` return
; leaves; simplifycfg tail merging folds them. Here two identical return
; tails merge back into one join.

define i32 @t(i1 %c, i32 %x) {
; CHECK-LABEL: define i32 @t(
entry:
  br i1 %c, label %A, label %B
A:
  %a1 = add i32 %x, 1, !revenant.va !{i64 4096}
  %a2 = mul i32 %a1, 3, !revenant.va !{i64 4097}
  ret i32 %a2
B:
  %b1 = add i32 %x, 1, !revenant.va !{i64 4112}
  %b2 = mul i32 %b1, 3, !revenant.va !{i64 4113}
  ret i32 %b2
; CHECK: ret i32
; CHECK-NOT: ret i32
}
