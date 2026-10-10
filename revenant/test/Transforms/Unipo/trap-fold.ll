; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; Dead ends prove nothing about side effects: the stubs are distinct
; declarations, and linked definitions may exit with distinct codes (here
; exit(2) vs exit(1)), so lli changes status even though both results are
; unused and unreachable follows. No fold; both functions survive.
; CHECK: define internal i32 @trap_a(i32 %x)
; CHECK: call void @sub_0x7000()
; CHECK: define internal i32 @trap_b(i32 %x)
; CHECK: call void @sub_0x7001()
; CHECK: define i32 @main()
; CHECK: call i32 @trap_b(i32 0)

@flag_a = internal constant i8 1
@flag_b = internal constant i8 1

declare void @sub_0x7000()
declare void @sub_0x7001()

define internal i32 @trap_a(i32 %x) {
entry:
  %f = load i8, ptr @flag_a
  %e = zext i8 %f to i32
  %v = add i32 %e, %x
  call void @sub_0x7000()
  unreachable
}

define internal i32 @trap_b(i32 %x) {
entry:
  %f = load i8, ptr @flag_b
  %e = zext i8 %f to i32
  %v = add i32 %e, %x
  call void @sub_0x7001()
  unreachable
}

define i32 @main() {
entry:
  %result = call i32 @trap_b(i32 0)
  ret i32 %result
}
