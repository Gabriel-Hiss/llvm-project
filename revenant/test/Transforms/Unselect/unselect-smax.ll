; Corpus: c-clang-o1 artifacts/main.post.ll @minmax (ir-pre: ternary
; min/max/abs if-ladders in main.pre.ll). llvm.smax expands to cmp+select.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unselect -S %s | %FileCheck %s

declare i32 @llvm.smax.i32(i32, i32)

define i32 @t(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @t(
entry:
  %m = call i32 @llvm.smax.i32(i32 %a, i32 %b), !revenant.va !{i64 4096}
  ret i32 %m
; The intrinsic unfolds to a compare plus a select, which then becomes a
; diamond in the same fixed-point run.
; CHECK: %m.c = icmp sgt i32 %a, %b
; CHECK: br i1 %m.c, label %entry.t, label %entry.f
; CHECK: entry.merge:
; CHECK: %m.sel.if = phi i32 [ %a, %entry.t ], [ %b, %entry.f ]
; CHECK: ret i32 %m.sel.if
}
