; RUN: %revenant-opt -passes=revenant-unsroa -S %s | %FileCheck %s
;
; Veto: a byte GEP chained under a variable-index GEP is a walking
; address, not a field. c-clang-o2 @hash_vec walks an i32 window with a
; variable index; the +4 byte step on top must not gain a struct label.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"

%struct.Point = type { i32, i32, ptr }

@__const.p = private unnamed_addr constant %struct.Point { i32 3, i32 4, ptr null }, align 8

define i32 @hash_walk(ptr %base, i64 %i) {
; CHECK-LABEL: define i32 @hash_walk(
bb:
  %w = getelementptr inbounds i32, ptr %base, i64 %i
  %b = getelementptr inbounds i8, ptr %w, i64 4
; CHECK: getelementptr inbounds i8, ptr %w, i64 4
; CHECK-NOT: getelementptr inbounds %struct.Point
  %v = load i32, ptr %b, align 4, !revenant.va !{i64 28672}
  ret i32 %v
}
