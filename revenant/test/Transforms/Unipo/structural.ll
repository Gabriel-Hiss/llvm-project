; RUN: opt -load-pass-plugin %unipo_plugin -passes=revenant-unipo,verify -S %s | FileCheck %s
; Corpus-derived shape: c-clang-o2lto/artifacts/main.post.ll, cmp_unsigned.
; Specialization fixtures below replace its second operand by 7 and strip all
; source symbols, as lifted IR does. They are controlled specializations of a
; real body, not claims that this particular binary contains these clones.
target triple = "x86_64-pc-windows-msvc"
; CHECK-NOT: define {{.*}}@sub_2000
; CHECK-NOT: define {{.*}}@sub_3000
; CHECK: define i32 @sub_1000(i32 %x, i32 %p)
; CHECK: define i32 @caller
; CHECK: call i32 @sub_1000(i32 %x, i32 7), !revenant.va
; CHECK: call i32 @sub_1000(i32 %x, i32 7), !revenant.va
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

; A missing middle parameter must preserve the order of the remaining formals.
; The real scalar add/sub body is c-clang-o2lto main.pre.ll, add_op/sub_op.
; CHECK-NOT: define {{.*}}@middle_clone
; CHECK: define i32 @middle_caller
; CHECK: call i32 @middle(i32 %x, i64 9, i32 %y)
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
