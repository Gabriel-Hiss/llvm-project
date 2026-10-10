; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; True trap tails still fold: the bodies differ only by dedup'd string data
; (same bytes, addresses never compared) and by `sub_0x` stubs that are
; dead ends in both bodies (results unused, unreachable follows). No caller
; can observe which stub fired, so the clone folds back.
; CHECK: define i32 @trap_a(i32 %x)
; CHECK: call void @sub_0x7000(ptr @str_a)
; CHECK-NOT: define i32 @trap_b
; CHECK: define i32 @trap_caller
; CHECK: call i32 @trap_a(i32 %x)

@str_a = internal constant [4 x i8] c"abc\00"
@str_b = internal constant [4 x i8] c"abc\00"

declare void @sub_0x7000(ptr)
declare void @sub_0x7001(ptr)

define i32 @trap_a(i32 %x) {
  call void @sub_0x7000(ptr @str_a)
  unreachable
}

define i32 @trap_b(i32 %x) {
  call void @sub_0x7001(ptr @str_b)
  unreachable
}

define i32 @trap_caller(i32 %x) {
  %v = call i32 @trap_b(i32 %x)
  ret i32 %v
}
