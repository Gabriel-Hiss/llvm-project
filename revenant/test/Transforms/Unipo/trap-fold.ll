; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; True trap tails still fold: the bodies differ only by dedup'd flag data
; (same bytes, loaded but never address-compared) and by `sub_0x` stubs
; that are dead ends in both bodies (results unused, unreachable follows).
; No caller can observe which flag object or stub fired, so the clone
; folds back.
; CHECK: define i32 @trap_a(i32 %x)
; CHECK: call void @sub_0x7000()
; CHECK-NOT: define i32 @trap_b
; CHECK: define i32 @trap_caller
; CHECK: call i32 @trap_a(i32 %x)

@flag_a = internal constant i8 1
@flag_b = internal constant i8 1

declare void @sub_0x7000()
declare void @sub_0x7001()

define i32 @trap_a(i32 %x) {
  %f = load i8, ptr @flag_a
  %e = zext i8 %f to i32
  %v = add i32 %e, %x
  call void @sub_0x7000()
  unreachable
}

define i32 @trap_b(i32 %x) {
  %f = load i8, ptr @flag_b
  %e = zext i8 %f to i32
  %v = add i32 %e, %x
  call void @sub_0x7001()
  unreachable
}

define i32 @trap_caller(i32 %x) {
  %v = call i32 @trap_b(i32 %x)
  ret i32 %v
}
