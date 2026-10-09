; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unipo,verify -S %s | %FileCheck %s
; Real lifted shape: c-gcc-o2.ll, sub_140001017 bb_140001037:
; store i64 2, ptr %rcx; load i64, ptr %rcx; call @sub_140003A10().
; Names below keep the stripped register-slot convention. Multiple callers and
; conflicting/barrier cases are controlled variants of that observed sequence.
target triple = "x86_64-pc-windows-msvc"

; CHECK: define void @sub_140003A10(i64 %ipa0)
define void @sub_140003A10() {
  ret void
}

; CHECK: define void @first
; CHECK: call void @sub_140003A10(i64 2), !revenant.va
; CHECK: call void @sub_140003A10(i64 2), !revenant.va
define void @first() {
  %rcx = alloca i64
  %sink = alloca i64
  store i64 2, ptr %rcx
  %r29 = load i64, ptr %rcx
  store i64 %r29, ptr %sink
  call void @sub_140003A10(), !revenant.va !0
  store i64 2, ptr %rcx
  call void @sub_140003A10(), !revenant.va !1
  ret void
}

; CHECK: define void @second
; CHECK: call void @sub_140003A10(i64 2)
define void @second() {
  %rcx = alloca i64
  store i64 2, ptr %rcx
  call void @sub_140003A10()
  ret void
}

; Unanimity is required even though adding an unused formal is equivalent.
; CHECK: define void @conflict()
define void @conflict() {
  ret void
}

define void @conflicting_calls() {
  %rcx = alloca i64
  store i64 3, ptr %rcx
  call void @conflict()
  store i64 4, ptr %rcx
  call void @conflict()
  ret void
}

; A call clobbers registers, so an earlier store does not justify restoration.
; CHECK: define void @after_call()
define void @after_call() {
  ret void
}

declare void @clobber()

define void @barrier() {
  %rcx = alloca i64
  store i64 2, ptr %rcx
  call void @clobber()
  call void @after_call()
  ret void
}

; The next argument register is rdx when rcx is already a formal.
; CHECK: define i64 @one_arg(i64 %a, i64 %ipa1)
define i64 @one_arg(i64 %a) {
  ret i64 %a
}

; CHECK: define i64 @extra_rdx
; CHECK: call i64 @one_arg(i64 %x, i64 17)
define i64 @extra_rdx(i64 %x) {
  %rdx = alloca i64
  store i64 17, ptr %rdx
  %v = call i64 @one_arg(i64 %x)
  ret i64 %v
}

; Cross-block stores are intentionally not guessed as reaching definitions.
; CHECK: define void @cross_block()
define void @cross_block() {
  ret void
}

define void @cross_block_caller() {
entry:
  %rcx = alloca i64
  store i64 2, ptr %rcx
  br label %next
next:
  call void @cross_block()
  ret void
}

!0 = !{i64 5368713271}
!1 = !{i64 5368713275}

; Clone-only recovery. The general definition is absent, as in GCC constprop
; output. The scalar add shape comes from c-clang-o2lto add_op; i64 widths and
; rdx stores use the lifted ABI convention.
; CHECK: define i64 @clone_only(i64 %x, i64 %ipa1)
; CHECK: %v = add i64 %x, %ipa1, !revenant.va
define i64 @clone_only(i64 %x) {
  %v = add i64 %x, 5, !revenant.va !0
  ret i64 %v
}

; CHECK: define i64 @clone_only_caller
; CHECK: call i64 @clone_only(i64 %x, i64 5), !revenant.va
define i64 @clone_only_caller(i64 %x) {
  %rdx = alloca i64
  store i64 5, ptr %rdx
  %v = call i64 @clone_only(i64 %x), !revenant.va !1
  ret i64 %v
}

; Constant-only switch operands must remain valid LLVM IR after restoration.
; The switch layout is the corpus c-clang-o2lto switch_sparse shape, reduced
; to a single arm to isolate the constant-only operand constraint.
; CHECK: define i64 @constant_case(i64 %x, i64 %ipa1)
; CHECK: switch i64 %x
; CHECK: i64 5, label %hit
define i64 @constant_case(i64 %x) {
entry:
  switch i64 %x, label %miss [i64 5, label %hit]
hit:
  ret i64 5
miss:
  ret i64 0
}

define i64 @constant_case_caller(i64 %x) {
  %rdx = alloca i64
  store i64 5, ptr %rdx
  %v = call i64 @constant_case(i64 %x)
  ret i64 %v
}

; GCC pool constprop keeps the object global inline but retains a caller-
; specific size argument. Each site must preserve its own size when the
; folded object formal is restored.
; CHECK: define i64 @pool_clone(i64 %ipa0, i64 %n)
; CHECK: define i64 @pool_caller(i64 %x, i64 %y)
; CHECK: call i64 @pool_clone(i64 ptrtoint (ptr @pool_data to i64), i64 11)
; CHECK: call i64 @pool_clone(i64 ptrtoint (ptr @pool_data to i64), i64 29)
@pool_data = internal constant [8 x i8] zeroinitializer, !revenant.va !6
declare void @pool_touch(i64)

define i64 @pool_clone(i64 %n) {
  call void @pool_touch(i64 ptrtoint (ptr @pool_data to i64))
  call void @pool_touch(i64 ptrtoint (ptr @pool_data to i64))
  ret i64 %n
}

define i64 @pool_caller(i64 %x, i64 %y) {
  %a = call i64 @pool_clone(i64 11)
  %b = call i64 @pool_clone(i64 29)
  %r = add i64 %a, %b
  %s = add i64 %r, %x
  %t = add i64 %s, %y
  ret i64 %t
}

!6 = !{i64 8400}
