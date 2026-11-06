; RUN: %revenant-opt -passes='function(revenant-o0-slots),verify' -S %s | %FileCheck %s
; An unoptimized function at the end of cleanup: its frame slots kept every
; access (volatile, pinned by revenant.o0.keep). The pass restores them and
; removes what the compiler's -O0 IR never had.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

declare void @revenant.o0.keep(ptr)
declare i32 @g(i32)
declare void @h(i64)

; CHECK-LABEL: define i32 @unopt(
; CHECK-NOT: volatile
; CHECK-NOT: @revenant.o0.keep
; The parameter home stays, with one load per read as clang -O0 emits them.
; CHECK: %home = alloca i32
; CHECK-NOT: alloca
; CHECK: store i32 %a, ptr %home
; CHECK: %v = load i32, ptr %home
; A source subtraction instcombine had turned into `add -1`.
; CHECK: %w = sub i32 %v, 1
; CHECK: %c = call i32 @g(i32 %w)
; CHECK: %x = load i32, ptr %home
; The call result kept across the second call is the value itself.
; CHECK: %sum = add i32 %c, %x
; The outgoing stack argument is passed directly.
; CHECK: call void @h(i64 5)
; CHECK: ret i32 %sum
define i32 @unopt(i32 %a) "revenant.o0" {
  %push = alloca i64
  %home = alloca i32
  %spill = alloca i32
  %out = alloca i64
  call void @revenant.o0.keep(ptr %push)
  call void @revenant.o0.keep(ptr %home)
  call void @revenant.o0.keep(ptr %spill)
  call void @revenant.o0.keep(ptr %out)
  store volatile i64 undef, ptr %push
  store volatile i32 %a, ptr %home
  %v = load volatile i32, ptr %home
  %w = add i32 %v, -1
  %c = call i32 @g(i32 %w)
  store volatile i32 %c, ptr %spill
  %x = load volatile i32, ptr %home
  %s = load volatile i32, ptr %spill
  %sum = add i32 %s, %x
  store volatile i64 5, ptr %out
  %sa = load volatile i64, ptr %out, !revenant.stackarg !0
  call void @h(i64 %sa)
  %pop = load volatile i64, ptr %push
  ret i32 %sum
}

; Two parameters inside one 8-byte reservation (`push rax` in a leaf) split
; into one alloca each once the reservation is gone.
; CHECK-LABEL: define i32 @leaf(
; CHECK-COUNT-2: alloca i32
; CHECK-NOT: alloca
; CHECK: ret i32
define i32 @leaf(i32 %a, i32 %b) "revenant.o0" {
  %frame = alloca i64
  call void @revenant.o0.keep(ptr %frame)
  store volatile i64 undef, ptr %frame
  %hi = getelementptr i8, ptr %frame, i64 4
  store volatile i32 %b, ptr %hi
  store volatile i32 %a, ptr %frame
  %x = load volatile i32, ptr %frame
  %y = load volatile i32, ptr %hi
  %r = add i32 %x, %y
  %pop = load volatile i64, ptr %frame
  ret i32 %r
}

; An optimized function is left alone.
; CHECK-LABEL: define i32 @optimized(
; CHECK: add i32 %a, -1
define i32 @optimized(i32 %a) {
  %r = add i32 %a, -1
  ret i32 %r
}

!0 = !{}
