; Corpus: rust-o3 artifacts/corpus_rust.pre.ll @enum_area (ir-pre keeps
; `{v, of} = llvm.smul.with.overflow` plus extractvalue pairs; the
; optimizer CSEs the two extracts into one value use). The pass leaves
; existing with.overflow sites alone and never invents a declare (this
; opt build rejects synthesized intrinsic declares at verify).
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unrust -S %s | %FileCheck %s

target triple = "x86_64-pc-windows-msvc"

declare void @my_panic(ptr)

; Function Attrs: nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none)
declare { i64, i1 } @llvm.smul.with.overflow.i64(i64, i64) #0

attributes #0 = { nocallback nocreateundeforpoison nofree nosync nounwind speculatable willreturn memory(none) }

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

define i64 @ov(i64 %a, i64 %b, ptr %slot) {
; CHECK-LABEL: define i64 @ov(
entry:
  %o = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %a, i64 %b), !revenant.va !{i64 8192}
  %m = extractvalue { i64, i1 } %o, 0
  %f = extractvalue { i64, i1 } %o, 1
  store i64 %m, ptr %slot, align 8
  call void @my_panic(ptr %slot)
  ret i64 %m
; Existing checked sites survive untouched.
; CHECK: %o = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 %a, i64 %b)
; CHECK: %m = extractvalue { i64, i1 } %o, 0
; CHECK: %f = extractvalue { i64, i1 } %o, 1
}
