; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; Review wrong-code shape: two functions pass byte-identical globals to
; distinct `sub_0x` declarations. The declarations carry no bodies, so
; nothing proves they behave the same, and the stub results are observed
; (returned). Both functions and both call sites must survive untouched:
; after linking 1/2-returning implementations, lli returns 0 either way.
; CHECK: define internal i64 @read_a()
; CHECK: call i64 @sub_0x1000()
; CHECK: define internal i64 @read_b()
; CHECK: call i64 @sub_0x2000()
; CHECK: define i32 @main()
; CHECK: call i64 @read_a()
; CHECK: call i64 @read_b()

@g_a = internal constant i32 7, align 4
@g_b = internal constant i32 7, align 4

declare i64 @sub_0x1000()
declare i64 @sub_0x2000()

define internal i64 @observe(ptr %p) {
entry:
  %address = ptrtoint ptr %p to i64
  ret i64 %address
}

define internal i64 @read_a() {
entry:
  %seen = call i64 @observe(ptr @g_a)
  %result = call i64 @sub_0x1000()
  ret i64 %result
}

define internal i64 @read_b() {
entry:
  %seen = call i64 @observe(ptr @g_b)
  %result = call i64 @sub_0x2000()
  ret i64 %result
}

define i32 @main() {
entry:
  %a = call i64 @read_a()
  %b = call i64 @read_b()
  %same = icmp eq i64 %a, %b
  %result = zext i1 %same to i32
  ret i32 %result
}
