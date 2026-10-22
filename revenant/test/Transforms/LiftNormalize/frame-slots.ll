; revenant-frame-slots splits a lifted frame whose addresses escape: each
; escaped address starts an object reaching up to the next accessed offset,
; adjacent write-only slots of one type become one array, and the scalar
; slots no escape reaches are promoted. Reduced from cpp-clang-o0
; string_ops (0x140001310): a 32-byte std::string at frame+56 handed to
; its constructor and methods, a spilled this-pointer at +48, the argument
; home slot and EH state at +88/+96.
; RUN: %revenant-opt -passes=revenant-frame-slots -S %s | %FileCheck %s

declare i64 @ctor(ptr, ptr)
declare i64 @size(ptr)
declare void @dtor(i64)

; CHECK-LABEL: define i64 @string_ops(
; CHECK-NOT: %frame
; CHECK: %stack_56 = alloca [32 x i8]
; CHECK-NEXT: %stack_88 = alloca [2 x i64]
; CHECK-NOT: alloca
; CHECK: store i64 -2, ptr %{{[0-9]+}}
; CHECK-NEXT: store i64 %arg.bits, ptr %stack_88
; CHECK-NEXT: %this = ptrtoint ptr %stack_56 to i64
; CHECK-NEXT: call i64 @ctor(ptr %stack_56, ptr %0)
; CHECK-NEXT: %len = call i64 @size(ptr %stack_56)
; CHECK-NEXT: call void @dtor(i64 %this)
; CHECK-NEXT: ret i64 %len

define i64 @string_ops(ptr %0) {
entry:
  %frame = alloca [408 x i8], align 1
  %arg.bits = ptrtoint ptr %0 to i64
  %eh = getelementptr inbounds nuw i8, ptr %frame, i64 96
  store i64 -2, ptr %eh, align 1
  %home = getelementptr inbounds nuw i8, ptr %frame, i64 88
  store i64 %arg.bits, ptr %home, align 1
  %str = getelementptr inbounds nuw i8, ptr %frame, i64 56
  %this = ptrtoint ptr %str to i64
  %spill = getelementptr inbounds nuw i8, ptr %frame, i64 48
  store i64 %this, ptr %spill, align 1
  %c = call i64 @ctor(ptr %str, ptr %0)
  %reload = load i64, ptr %spill, align 1
  %len = call i64 @size(ptr %str)
  %res = getelementptr inbounds nuw i8, ptr %frame, i64 40
  store i64 %len, ptr %res, align 1
  call void @dtor(i64 %reload)
  %out = load i64, ptr %res, align 1
  ret i64 %out
}

; Fields written right after the escaped address stay in its object (a
; struct filled, then handed out); a run of write-only slots of one type
; becomes one array; a reloaded scalar spill is promoted.
declare void @use(ptr)

; CHECK-LABEL: define i64 @init_pass(
; CHECK: %stack_16 = alloca [16 x i8]
; CHECK-NEXT: %stack_32 = alloca [4 x i32]
; CHECK-NOT: alloca
; CHECK: store i32 1, ptr %stack_16
; CHECK: store i32 2, ptr %[[Y:[0-9]+]]
; CHECK: call void @use(ptr %stack_16)
; CHECK: ret i64 7

define i64 @init_pass() {
entry:
  %frame = alloca [64 x i8], align 1
  %x = getelementptr inbounds nuw i8, ptr %frame, i64 16
  store i32 1, ptr %x, align 1
  %y = getelementptr inbounds nuw i8, ptr %frame, i64 20
  store i32 2, ptr %y, align 1
  %a0 = getelementptr inbounds nuw i8, ptr %frame, i64 32
  store i32 0, ptr %a0, align 1
  %a1 = getelementptr inbounds nuw i8, ptr %frame, i64 36
  store i32 0, ptr %a1, align 1
  %a2 = getelementptr inbounds nuw i8, ptr %frame, i64 40
  store i32 0, ptr %a2, align 1
  %a3 = getelementptr inbounds nuw i8, ptr %frame, i64 44
  store i32 0, ptr %a3, align 1
  call void @use(ptr %x)
  %s = getelementptr inbounds nuw i8, ptr %frame, i64 48
  store i64 7, ptr %s, align 1
  %v = load i64, ptr %s, align 1
  ret i64 %v
}

; A frame without escapes is the cleanup's job: lift-normalize already
; split and promoted it, so the pass leaves other allocas alone.
; CHECK-LABEL: define i32 @plain(
; CHECK: %x = alloca i32
; CHECK-NEXT: store i32 1, ptr %x

define i32 @plain() {
entry:
  %x = alloca i32, align 4
  store i32 1, ptr %x, align 4
  %v = load i32, ptr %x, align 4
  ret i32 %v
}
