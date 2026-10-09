; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Corpus: c-o2 sub_140001620 (main): the caller builds `struct Point p`
; inside the lifter's [728 x i8] frame at offsets 312..327, then passes
; its address to struct_access. A stack-cookie i64 sits at frame+328 and
; an unrelated i8 peeks at frame+315 (inside the i32 field at 312): the
; window still clusters, the peek is left alone, and the cookie store is
; retitled like the rest of the observed window.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

declare i32 @callee(i64)

; CHECK: %struct.agg.f.0 = type { i32, i32, i64 }
define i32 @f(i32 %0) {
; CHECK-LABEL: define i32 @f(
bb:
  %frame = alloca [728 x i8], align 1
  %fp32 = getelementptr inbounds nuw i8, ptr %frame, i64 8
  %rsp33 = ptrtoint ptr %fp32 to i64
  %1 = xor i64 %rsp33, 47936899621426
  %element = getelementptr inbounds nuw i64, ptr %frame, i64 41
  store i64 %1, ptr %element, align 1
; CHECK: %rv.agg.base = getelementptr i8, ptr %frame, i64 312
  %fp47 = getelementptr inbounds nuw i8, ptr %frame, i64 312
  %4 = ptrtoint ptr %fp47 to i64
  %fp49 = getelementptr inbounds nuw i8, ptr %frame, i64 320
  store i64 1234, ptr %fp49, align 1
  store i32 3, ptr %fp47, align 1
  %element233 = getelementptr inbounds nuw i32, ptr %frame, i64 79
  store i32 4, ptr %element233, align 1
  %fp696 = getelementptr inbounds nuw i8, ptr %frame, i64 315
  %35 = load i8, ptr %fp696, align 1
; CHECK: getelementptr inbounds nuw %struct.agg.f.0, ptr %rv.agg.base, i32 0, i32 2
; CHECK: getelementptr inbounds nuw %struct.agg.f.0, ptr %rv.agg.base, i32 0, i32 0
; CHECK: getelementptr inbounds nuw %struct.agg.f.0, ptr %rv.agg.base, i32 0, i32 1
; CHECK: %{{.*}} = load i8, ptr %fp696, align 1
  %c = call i32 @callee(i64 %4)
  ret i32 %c
}
