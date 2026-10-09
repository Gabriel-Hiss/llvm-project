; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-uncxx -S %s | %FileCheck %s
; Regression cases for MSVC's overlapping vector-end/string-size offset.
; A real vector empty check compares the vector's first and last fields.
; A loop exit compares an unrelated iterator with a loaded vector end.
; CHECK-LABEL: define i1 @vector_empty
; CHECK: call i1 @revenant.cxx.vector.empty(ptr %v)
; CHECK-LABEL: define i1 @loop_exit
; CHECK-NOT: revenant.cxx.vector.empty
; CHECK: icmp eq ptr %it, %end
; CHECK-LABEL: define i64 @vector_end_not_string_size
; CHECK-NOT: revenant.cxx.string.size
; CHECK: load ptr, ptr %end.addr

 target triple = "x86_64-pc-windows-msvc"

define i1 @vector_empty(ptr %v) {
entry:
  %first.addr = getelementptr i8, ptr %v, i64 0
  %last.addr = getelementptr i8, ptr %v, i64 8
  %end.addr = getelementptr i8, ptr %v, i64 16
  %first = load ptr, ptr %first.addr, align 8
  %last = load ptr, ptr %last.addr, align 8
  %end = load ptr, ptr %end.addr, align 8
  %empty = icmp eq ptr %first, %last
  %keep.vector.shape = icmp ne ptr %end, null
  %result = and i1 %empty, %keep.vector.shape
  ret i1 %result
}

define i1 @loop_exit(ptr %v, ptr %it) {
entry:
  %end.addr = getelementptr i8, ptr %v, i64 16
  %end = load ptr, ptr %end.addr, align 8
  %done = icmp eq ptr %it, %end
  ret i1 %done
}

define i64 @vector_end_not_string_size(ptr %v) {
entry:
  %first.addr = getelementptr i8, ptr %v, i64 0
  %last.addr = getelementptr i8, ptr %v, i64 8
  %end.addr = getelementptr i8, ptr %v, i64 16
  %extra.addr = getelementptr i8, ptr %v, i64 24
  %first = load ptr, ptr %first.addr, align 8
  %last = load ptr, ptr %last.addr, align 8
  %end = load ptr, ptr %end.addr, align 8
  %extra = load i64, ptr %extra.addr, align 8
  %value = ptrtoint ptr %end to i64
  %use = add i64 %value, %extra
  ret i64 %use
}
; CHECK-LABEL: define i32 @vector_back
; CHECK: call ptr @revenant.cxx.vector.back(ptr %v)

define i32 @vector_back(ptr %v) {
entry:
  %first.addr = getelementptr i8, ptr %v, i64 0
  %last.addr = getelementptr i8, ptr %v, i64 8
  %end.addr = getelementptr i8, ptr %v, i64 16
  %first = load ptr, ptr %first.addr, align 8
  %last = load ptr, ptr %last.addr, align 8
  %end = load ptr, ptr %end.addr, align 8
  %last.value.addr = getelementptr i32, ptr %last, i64 -1
  %value = load i32, ptr %last.value.addr, align 4
  %keep.shape = icmp ne ptr %first, %end
  %result = select i1 %keep.shape, i32 %value, i32 0
  ret i32 %result
}
declare void @"?vector@std@@"()
; CHECK-LABEL: define ptr @string_null_fallback
; CHECK-NOT: revenant.cxx.string.data
; CHECK: select i1 %is.null, ptr @fallback, ptr %data

@fallback = private constant [5 x i8] c"error"

define ptr @string_null_fallback(ptr %s) {
entry:
  %data.addr = getelementptr i8, ptr %s, i64 0
  %data = load ptr, ptr %data.addr, align 8
  %is.null = icmp eq ptr %data, null
  %result = select i1 %is.null, ptr @fallback, ptr %data
  ret ptr %result
}
; CHECK-LABEL: define ptr @string_sso_data
; CHECK: call ptr @revenant.cxx.string.data(ptr %s)

define ptr @string_sso_data(ptr %s) {
entry:
  %data.addr = getelementptr i8, ptr %s, i64 0
  %cap.addr = getelementptr i8, ptr %s, i64 24
  %data = load ptr, ptr %data.addr, align 8
  %cap = load i64, ptr %cap.addr, align 8
  %short = icmp ult i64 %cap, 16
  %result = select i1 %short, ptr %s, ptr %data
  ret ptr %result
}
; CHECK-LABEL: define i64 @vector_back_byte_offset
; CHECK: call ptr @revenant.cxx.vector.back

define i64 @vector_back_byte_offset(ptr %v) {
entry:
  %first.addr = getelementptr i8, ptr %v, i64 0
  %last.addr = getelementptr i8, ptr %v, i64 8
  %first = load ptr, ptr %first.addr, align 8
  %last = load ptr, ptr %last.addr, align 8
  %back.addr = getelementptr i8, ptr %last, i64 -16
  %field.addr = getelementptr i8, ptr %back.addr, i64 8
  %field = load i64, ptr %field.addr, align 8
  %keep.shape = icmp ne ptr %first, %last
  %result = select i1 %keep.shape, i64 %field, i64 0
  ret i64 %result
}
; CHECK-LABEL: define i1 @integer_fields_equal
; CHECK-NOT: revenant.cxx.vector.empty
; CHECK: icmp eq i32 %first, %last

define i1 @integer_fields_equal(ptr %record) {
entry:
  %first = load i32, ptr %record, align 4
  %last.addr = getelementptr i8, ptr %record, i64 8
  %last = load i32, ptr %last.addr, align 4
  %equal = icmp eq i32 %first, %last
  ret i1 %equal
}
!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"cpp"}
