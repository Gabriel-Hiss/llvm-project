; Corpus: c-clang-o1 artifacts/main.post.ll @cmp_signed (ir-pre: the
; `if (a<b) return -1; if (a>b) return 1; return 0` ladder in main.pre.ll).
; llvm.scmp unfolds to the nested select the ladder lowers to, and each
; level becomes a diamond in the same fixed-point run.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unselect -S %s | %FileCheck %s

declare i32 @llvm.scmp.i32.i32(i32, i32)

define i32 @t(i32 %a, i32 %b) {
; CHECK-LABEL: define i32 @t(
  %s = call i32 @llvm.scmp.i32.i32(i32 %a, i32 %b), !revenant.va !{i64 4096}
  ret i32 %s
; CHECK: %s.lt = icmp slt i32 %a, %b
; CHECK: %s.gt = icmp sgt i32 %a, %b
; CHECK: %s.hi.if = phi i32 [ 1, %.t2 ], [ 0, %.f3 ]
; CHECK: %s.sel.if = phi i32 [ -1, %.t ], [ %s.hi.if, %.f ]
; CHECK: ret i32 %s.sel.if
}
