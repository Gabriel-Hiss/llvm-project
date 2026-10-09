; RUN: %revenant-opt -passes=revenant-unipo,verify -S %s | %FileCheck %s
; Same bodies after constant substitution are not proof of a shared source
; function. Keep both signatures and preserve their direct calls.
target triple = "x86_64-pc-windows-msvc"
; CHECK: define i32 @sub_1000(i32 %x, i32 %p)
; CHECK: define i32 @sub_2000(i32 %a)
; CHECK: define i32 @sub_3000(i32 %a, i32 %unused)
; CHECK: define i32 @caller
; CHECK: call i32 @sub_2000(i32 %x), !revenant.va
; CHECK: call i32 @sub_3000(i32 %x, i32 7), !revenant.va
; CHECK: define i32 @different
; CHECK: define i32 @address_taken

define i32 @sub_1000(i32 %x, i32 %p) {
entry:
  %lt = icmp ult i32 %x, %p
  %eq = icmp eq i32 %x, %p
  %tail = select i1 %eq, i32 2, i32 3
  %out = select i1 %lt, i32 1, i32 %tail
  ret i32 %out
}

define i32 @sub_2000(i32 %a) {
stripped:
  %less = icmp ult i32 %a, 7
  %equal = icmp eq i32 %a, 7
  %t = select i1 %equal, i32 2, i32 3
  %v = select i1 %less, i32 1, i32 %t
  ret i32 %v
}

define i32 @sub_3000(i32 %a, i32 %unused) {
entry:
  %less = icmp ult i32 %a, 7
  %equal = icmp eq i32 %a, 7
  %t = select i1 %equal, i32 2, i32 3
  %v = select i1 %less, i32 1, i32 %t
  ret i32 %v
}

define i32 @caller(i32 %x) {
  %rdx = alloca i32
  store i32 7, ptr %rdx
  %a = call i32 @sub_2000(i32 %x), !revenant.va !0
  %b = call i32 @sub_3000(i32 %x, i32 7), !revenant.va !1
  %r = add i32 %a, %b
  ret i32 %r
}

; Exact comparison rejects an abstract-hash collision with different constants.
define i32 @different(i32 %x) {
  %lt = icmp ult i32 %x, 7
  %eq = icmp eq i32 %x, 7
  %t = select i1 %eq, i32 4, i32 5
  %v = select i1 %lt, i32 1, i32 %t
  ret i32 %v
}

define i32 @different_caller(i32 %x) {
  %v = call i32 @different(i32 %x)
  ret i32 %v
}

; Function pointer identity prevents deletion even for an identical body.
@callback = global ptr @address_taken

define i32 @address_taken(i32 %x) {
  %lt = icmp ult i32 %x, 7
  %eq = icmp eq i32 %x, 7
  %t = select i1 %eq, i32 2, i32 3
  %v = select i1 %lt, i32 1, i32 %t
  ret i32 %v
}

; A same-shaped body with fewer formals is ambiguous in stripped IR. Keep its
; signature and call rather than guessing that it is a compiler clone.
; CHECK: define i32 @middle(i32 %x, i64 %ignored, i32 %y)
; CHECK: define i32 @middle_clone(i32 %x, i32 %y)
; CHECK: call i32 @middle_clone(i32 %x, i32 %y)
define i32 @middle(i32 %x, i64 %ignored, i32 %y) {
  %r = sub i32 %x, %y
  ret i32 %r
}

define i32 @middle_clone(i32 %x, i32 %y) {
  %r = sub i32 %x, %y
  ret i32 %r
}

define i32 @middle_caller(i32 %x, i32 %y) {
  %rdx = alloca i64
  store i64 9, ptr %rdx
  %v = call i32 @middle_clone(i32 %x, i32 %y)
  ret i32 %v
}

!0 = !{i64 4096}
!1 = !{i64 4100}

; GCC .isra shape: pooled string globals have equal bytes but distinct lifted
; names, and the noreturn trap tail is emitted as two VA-specific stubs.
; Identical data does not make addresses interchangeable when code compares
; them. Both the specialized target and its caller must remain unchanged.
; CHECK: define i32 @sub_8000(i32 %x)
; CHECK: call i32 @sub_8000(i32 %x)
; CHECK-NOT: define i32 @sub_5000
; CHECK: define i32 @global_caller
; CHECK: call i32 @sub_4000(i32 %x)
; CHECK: define i32 @sub_9000(i32 %tag)
; CHECK: define i32 @sub_a000()
; CHECK: define void @part_cold.cold
; CHECK: define void @part_clone.cold
; CHECK: define void @startup_like(i32 %mode)
@addr_a = internal constant [4 x i8] c"abc\00", !revenant.va !4
@addr_b = internal constant [4 x i8] c"abc\00", !revenant.va !5

define i32 @sub_7000(i32 %x) {
  %same = icmp eq ptr @addr_a, null
  %v = select i1 %same, i32 %x, i32 0
  ret i32 %v
}

define i32 @sub_8000(i32 %x) {
  %same = icmp eq ptr @addr_b, null
  %v = select i1 %same, i32 %x, i32 0
  ret i32 %v
}

define i32 @address_caller(i32 %x) {
  %v = call i32 @sub_8000(i32 %x)
  ret i32 %v
}
@str_a = internal constant [4 x i8] c"abc\00", !revenant.va !2
@str_b = internal constant [4 x i8] c"abc\00", !revenant.va !3
declare void @sub_0x6000(ptr)
declare void @sub_0x6001(ptr)

define i32 @sub_4000(i32 %x) {
  call void @sub_0x6000(ptr @str_a)
  ret i32 %x
}

define i32 @sub_5000(i32 %x) {
  call void @sub_0x6001(ptr @str_b)
  ret i32 %x
}

define i32 @global_caller(i32 %x) {
  %v = call i32 @sub_5000(i32 %x)
  ret i32 %v
}


; A constant body literal is not evidence that a zero-argument function is a
; specialization of an unrelated live formal. The callers provide no proof.
; Both stay distinct: no reaching constant at a callsite justifies this fold.
define i32 @sub_9000(i32 %tag) {
  ret i32 %tag
}

define i32 @sub_a000() {
  ret i32 1
}

define i32 @literal_caller(i32 %x) {
  %v = call i32 @sub_a000()
  ret i32 %v
}
; Outlined cold regions are not function clones. Keep both even when the
; normalized bodies have the same shape and reference equivalent data.
define void @part_cold.cold() {
  call void @sub_0x6000(ptr @str_a)
  ret void
}

define void @part_clone.cold() {
  call void @sub_0x6001(ptr @str_b)
  ret void
}

; Repeated global constants in different ABI argument positions do not prove
; a folded `this` formal. This mirrors the LTO startup-helper false positive.
@config_buffer = internal constant [8 x i8] zeroinitializer
declare void @set_buffer(i64)
declare void @copy_to_path(i32, i64)
define void @startup_like(i32 %mode) {
  call void @set_buffer(i64 ptrtoint (ptr @config_buffer to i64))
  call void @copy_to_path(i32 %mode, i64 ptrtoint (ptr @config_buffer to i64))
  ret void
}

!2 = !{i64 8192}
!4 = !{i64 8300}
!5 = !{i64 8304}
!3 = !{i64 8200}
