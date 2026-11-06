; RUN: %revenant-opt -passes='revenant-lift-abi(instcombine<no-verify-fixpoint>),verify' -S %s | %FileCheck %s
; The nested passes run over the functions the ABI rewrite changed: the
; rewritten callee and its caller. A function the rewrite left alone is not
; touched by them.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

; CHECK-LABEL: define i32 @read(ptr
; CHECK-NOT: ptrtoint
; CHECK-NOT: inttoptr
; CHECK: load i32, ptr %address
define i32 @read(i64 %address) {
  %p = inttoptr i64 %address to ptr
  %v = load i32, ptr %p
  ret i32 %v
}

; CHECK-LABEL: define i32 @caller(ptr
; CHECK-NOT: ptrtoint
; CHECK: call i32 @read(ptr %base)
define i32 @caller(i64 %base) {
  %v = call i32 @read(i64 %base)
  ret i32 %v
}

; CHECK-LABEL: define i32 @untouched(
; CHECK: add i32 %x, 0
define i32 @untouched(i32 %x) {
  %y = add i32 %x, 0
  ret i32 %y
}
