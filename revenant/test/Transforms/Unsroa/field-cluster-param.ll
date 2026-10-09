; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Corpus: c-o2 sub_140001B00 (`struct_access(struct Point *p)`),
; Point = { i32 x, i32 y, const char *tag }. The caller constructs the
; three fields and passes the same base; E requires that cross-function
; evidence before synthesizing a type for the non-alloca parameter.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

; CHECK: %struct.agg.struct_access.0 = type { i32, i32, i64 }
define i32 @struct_access(ptr %base.ptr) {
; CHECK-LABEL: define i32 @struct_access(
bb_140001b00:
  %element32 = getelementptr i32, ptr %base.ptr, i64 1
  %1 = load i32, ptr %element32, align 1, !revenant.va !{i64 71681}
; CHECK: getelementptr %struct.agg.struct_access.0, ptr %base.ptr, i32 0, i32 1
  %2 = load i32, ptr %base.ptr, align 1, !revenant.va !{i64 71682}
; CHECK: getelementptr %struct.agg.struct_access.0, ptr %base.ptr, i32 0, i32 0
  %3 = add i32 %2, %1, !revenant.va !{i64 71682}
  store i32 %3, ptr %base.ptr, align 1, !revenant.va !{i64 71682}
  %element = getelementptr i64, ptr %base.ptr, i64 1
  %4 = load i64, ptr %element, align 1, !revenant.va !{i64 71683}
; CHECK: getelementptr %struct.agg.struct_access.0, ptr %base.ptr, i32 0, i32 2
  %c = icmp eq i64 %4, 0, !revenant.va !{i64 71684}
  br i1 %c, label %bb_out, label %bb_more, !revenant.va !{i64 71685}

bb_more:
  store i32 0, ptr %element32, align 1, !revenant.va !{i64 71686}
  br label %bb_out

bb_out:
  %r = phi i32 [ %1, %bb_140001b00 ], [ 0, %bb_more ]
  %narrow12 = mul i32 %3, %r, !revenant.va !{i64 71687}
  ret i32 %narrow12, !revenant.va !{i64 71684}
}

define i32 @main(ptr %p) {
  %p1 = getelementptr i8, ptr %p, i64 4
  store i32 3, ptr %p
  store i32 4, ptr %p1
  %p2 = getelementptr i8, ptr %p, i64 8
  store i64 0, ptr %p2
  %r = call i32 @struct_access(ptr %p)
  ret i32 %r
}
