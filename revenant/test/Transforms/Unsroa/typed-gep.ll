; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Corpus: c-clang-o2, @struct_access. instcombine flattens the typed field
; GEPs of ir-pre into byte GEPs (`gep i8, ptr %0, i64 4`); the pass rebuilds
; the typed field GEP (`gep %struct.Point, ptr %0, i32 0, i32 1`).

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

%struct.Point = type { i32, i32, ptr }

@__const.p = private unnamed_addr constant %struct.Point { i32 3, i32 4, ptr null }, align 8

define i32 @struct_access(ptr noundef %0) {
; CHECK-LABEL: define i32 @struct_access(
  %2 = getelementptr inbounds i8, ptr %0, i64 4, !revenant.va !{i64 4096}
; CHECK: getelementptr inbounds %struct.Point, ptr %0, i32 0, i32 1
  %3 = load i32, ptr %2, align 4, !revenant.va !{i64 4097}
  %4 = load i32, ptr %0, align 8, !revenant.va !{i64 4098}
  %5 = add i32 %4, %3, !revenant.va !{i64 4099}
  store i32 %5, ptr %0, align 8, !revenant.va !{i64 4100}
  %6 = getelementptr inbounds i8, ptr %0, i64 8, !revenant.va !{i64 4101}
; CHECK: getelementptr inbounds %struct.Point, ptr %0, i32 0, i32 2
  %7 = load ptr, ptr %6, align 8, !revenant.va !{i64 4102}
  ret i32 %5
}
