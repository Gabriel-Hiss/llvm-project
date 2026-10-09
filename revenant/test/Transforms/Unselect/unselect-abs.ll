; Corpus: c-clang-o1 artifacts/main.post.ll @minmax abs fold
; (ir-pre: `a < 0 ? -a : a` ladder in main.pre.ll). llvm.abs expands to
; the slt compare plus negate select.
; RUN: %revenant-opt -passes=revenant-unselect -S %s | %FileCheck %s

declare i32 @llvm.abs.i32(i32, i1 immarg)

define i32 @t(i32 %a) {
; CHECK-LABEL: define i32 @t(
entry:
  %m = call i32 @llvm.abs.i32(i32 %a, i1 true), !revenant.va !{i64 4096}
  ret i32 %m
; CHECK: %m.c = icmp slt i32 %a, 0
; CHECK: br i1 %m.c, label %entry.t, label %entry.f
; CHECK: entry.merge:
; CHECK: %m.sel.if = phi i32 [ %m.neg, %entry.t ], [ %a, %entry.f ]
; CHECK: ret i32 %m.sel.if
}
