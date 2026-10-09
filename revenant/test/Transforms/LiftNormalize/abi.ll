; RUN: %revenant-opt -passes='revenant-lift-abi,function(revenant-lift-normalize,instcombine<no-verify-fixpoint>),verify' -S %s | %FileCheck %s
; Lifted memory bases and returned addresses are kept in i64 registers.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"
; CHECK-LABEL: define i32 @read(ptr
; CHECK-NOT: inttoptr
; CHECK: load i32
define i32 @read(i64 %address) {
  %p = inttoptr i64 %address to ptr
  %v = load i32, ptr %p
  ret i32 %v
}
; CHECK-LABEL: declare ptr @alloc()
declare i64 @alloc()
; CHECK-LABEL: define i32 @caller(ptr
; CHECK-DAG: call ptr @alloc()
; CHECK-DAG: call i32 @read(ptr
define i32 @caller(i64 %base) {
  %v = call i32 @read(i64 %base)
  %bits = call i64 @alloc()
  %p = inttoptr i64 %bits to ptr
  store i32 %v, ptr %p
  ret i32 %v
}
; CHECK-LABEL: define ptr @field(ptr
; CHECK: load ptr
; CHECK-NOT: inttoptr
define i64 @field(i64 %base) {
  %p = inttoptr i64 %base to ptr
  %bits = load i64, ptr %p
  %address = inttoptr i64 %bits to ptr
  store i8 0, ptr %address
  ret i64 %bits
}
define void @field_user(i64 %base) {
  %bits = call i64 @field(i64 %base)
  %p = inttoptr i64 %bits to ptr
  store i8 1, ptr %p
  ret void
}
