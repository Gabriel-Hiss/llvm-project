; RUN: %revenant-opt -passes=revenant-copyruns,verify -S %s | %FileCheck %s
; The backend expands a small fixed-size memcpy into loads and stores that
; move one contiguous range between two bases. Compiler IR has the memcpy.

; CHECK-LABEL: define void @copy24(
; CHECK: %[[S:.*]] = getelementptr i8, ptr %frame, i64 72
; CHECK-NEXT: call void @llvm.memcpy.p0.p0.i64(ptr align 1 %dst, ptr align 1 %[[S]], i64 24, i1 false)
; CHECK-NOT: load
; CHECK: ret void
define void @copy24(ptr %dst) {
  %frame = alloca [128 x i8]
  %s0 = getelementptr i8, ptr %frame, i64 72
  %a = load i64, ptr %s0
  store i64 %a, ptr %dst
  %s1 = getelementptr i8, ptr %frame, i64 80
  %b = load i64, ptr %s1
  %d1 = getelementptr i8, ptr %dst, i64 8
  store i64 %b, ptr %d1
  %s2 = getelementptr i8, ptr %frame, i64 88
  %c = load i64, ptr %s2
  %d2 = getelementptr i8, ptr %dst, i64 16
  store i64 %c, ptr %d2
  ret void
}

; Two fields, eight bytes: an ordinary pair of assignments.
; CHECK-LABEL: define void @fields(
; CHECK-NOT: memcpy
; CHECK: ret void
define void @fields(ptr %dst, ptr %src) {
  %a = load i32, ptr %src
  store i32 %a, ptr %dst
  %s1 = getelementptr i8, ptr %src, i64 4
  %b = load i32, ptr %s1
  %d1 = getelementptr i8, ptr %dst, i64 4
  store i32 %b, ptr %d1
  ret void
}

; A call between the pairs may read or write either range.
; CHECK-LABEL: define void @interrupted(
; CHECK-NOT: memcpy
; CHECK: ret void
define void @interrupted(ptr %dst, ptr %src) {
  %a = load i64, ptr %src
  store i64 %a, ptr %dst
  call void @g()
  %s1 = getelementptr i8, ptr %src, i64 8
  %b = load i64, ptr %s1
  %d1 = getelementptr i8, ptr %dst, i64 8
  store i64 %b, ptr %d1
  ret void
}

; The second store does not land at the offset its load came from.
; CHECK-LABEL: define void @shuffled(
; CHECK-NOT: memcpy
; CHECK: ret void
define void @shuffled(ptr %dst, ptr %src) {
  %a = load i64, ptr %src
  %d1 = getelementptr i8, ptr %dst, i64 8
  store i64 %a, ptr %d1
  %s1 = getelementptr i8, ptr %src, i64 8
  %b = load i64, ptr %s1
  store i64 %b, ptr %dst
  ret void
}

; Zero stores over 24 contiguous bytes: an expanded memset.
; CHECK-LABEL: define void @zero24(
; CHECK-NEXT: call void @llvm.memset.p0.i64(ptr align 1 %p, i8 0, i64 24, i1 false)
; CHECK-NEXT: ret void
define void @zero24(ptr %p) {
  store i64 0, ptr %p
  %p1 = getelementptr i8, ptr %p, i64 8
  store i64 0, ptr %p1
  %p2 = getelementptr i8, ptr %p, i64 16
  store i64 0, ptr %p2
  ret void
}

; Different values per field: ordinary initialization.
; CHECK-LABEL: define void @init(
; CHECK-NOT: memset
; CHECK: ret void
define void @init(ptr %p) {
  store i64 0, ptr %p
  %p1 = getelementptr i8, ptr %p, i64 8
  store i64 1, ptr %p1
  %p2 = getelementptr i8, ptr %p, i64 16
  store i64 0, ptr %p2
  ret void
}

declare void @g()
