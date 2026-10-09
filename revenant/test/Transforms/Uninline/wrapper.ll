; Corpus shape: miniz-clang-o1 `mz_deflateInit` (ir-pre) is a single-call
; wrapper tail-calling `mz_deflateInit2`; ir-post `mz_compress2` inlines
; that call. The pass recovers the wrapper call from the bound inner call.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uninline -S %s | %FileCheck %s

declare i32 @mz_deflateInit2(ptr noundef, i32 noundef, i32 noundef, i32 noundef, i32 noundef, i32 noundef)

; CHECK-LABEL: define i32 @mz_deflateInit(
define i32 @mz_deflateInit(ptr noundef %0, i32 noundef %1) {
  %3 = tail call i32 @mz_deflateInit2(ptr noundef %0, i32 noundef %1, i32 noundef 8, i32 noundef 15, i32 noundef 9, i32 noundef 0)
  ret i32 %3
}

; CHECK-LABEL: define i32 @mz_compress2(
; CHECK: %[[V:[0-9]+]] = call i32 @mz_deflateInit(ptr noundef %1, i32 noundef %2)
; CHECK: icmp eq i32 %[[V]], 0
define i32 @mz_compress2(ptr noundef %0, ptr noundef %1, i32 noundef %2) {
entry:
  ; Inlined copy of the wrapper body with %0->%1, %1->%2.
  %12 = call i32 @mz_deflateInit2(ptr noundef %1, i32 noundef %2, i32 noundef 8, i32 noundef 15, i32 noundef 9, i32 noundef 0), !revenant.va !{i64 5000}
  %13 = icmp eq i32 %12, 0, !revenant.va !{i64 5001}
  br i1 %13, label %ok, label %fail, !revenant.va !{i64 5001}

ok:
  ret i32 0

fail:
  ret i32 %12
}
