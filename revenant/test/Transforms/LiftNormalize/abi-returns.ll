; RUN: %revenant-opt -passes='revenant-lift-abi,verify' -S %s | %FileCheck %s
; Return width and pointer evidence the lifted machine code leaves out: the
; return register holds whatever the last callee left in it, a 32-bit write
; zero-extends rax, and an address may only ever be passed along.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

declare i64 @sink(i64)

; No caller reads the result and the function only hands back a callee's
; leftover rax: void.
; CHECK-LABEL: define void @leftover(
; CHECK: call i64 @sink
; CHECK-NEXT: ret void
define i64 @leftover(i64 %x) {
  %c = call i64 @sink(i64 %x)
  ret i64 %c
}
; CHECK-LABEL: define void @leftover_caller(
; CHECK: call void @leftover(
define void @leftover_caller(i64 %x) {
  %r = call i64 @leftover(i64 %x)
  ret void
}

; A value the function computes stays its result even though no caller
; reads it: the caller may have lost the use (a register kept across a call).
; CHECK-LABEL: define i64 @computed(
; CHECK: ret i64 %s
define i64 @computed(i64 %a, i64 %b) {
  %s = add i64 %a, %b
  ret i64 %s
}
define void @computed_caller() {
  %r = call i64 @computed(i64 1, i64 2)
  ret void
}

; A 32-bit result reaches rax zero-extended: the function returns the i32,
; and the caller extends it where it reads 64 bits.
; CHECK-LABEL: define i32 @narrow(
; CHECK: ret i32 %s
define i64 @narrow(i32 %a) {
  %s = add i32 %a, 1
  %z = zext i32 %s to i64
  ret i64 %z
}
; CHECK-LABEL: define i64 @narrow_caller(
; CHECK: %[[R:.+]] = call i32 @narrow(
; CHECK: zext i32 %[[R]] to i64
define i64 @narrow_caller(i32 %a) {
  %r = call i64 @narrow(i32 %a)
  ret i64 %r
}

; Returning the bits of a pointer returns a pointer.
; CHECK-LABEL: define ptr @address_of(
; CHECK: ret ptr
define i64 @address_of(ptr %p) {
  %e = getelementptr i8, ptr %p, i64 8
  %bits = ptrtoint ptr %e to i64
  ret i64 %bits
}
define void @address_user(ptr %p) {
  %bits = call i64 @address_of(ptr %p)
  %q = inttoptr i64 %bits to ptr
  store i8 0, ptr %q
  ret void
}

; A caller that passes the bits of a pointer makes the parameter a pointer.
; CHECK-LABEL: define void @takes(ptr
define void @takes(i64 %p) {
  %c = call i64 @sink(i64 %p)
  ret void
}
define void @passes(ptr %p) {
  %bits = ptrtoint ptr %p to i64
  call void @takes(i64 %bits)
  ret void
}

; A constructor hands back `this` whether or not the caller uses it: the
; result stays a pointer instead of becoming void.
; CHECK-LABEL: define ptr @ctor(ptr
define i64 @ctor(i64 %this) {
  %p = inttoptr i64 %this to ptr
  store i64 0, ptr %p
  ret i64 %this
}
define void @ctor_caller(i64 %this) {
  %r = call i64 @ctor(i64 %this)
  ret void
}

; An indirect call passes the pointer itself and reads a result that is only
; ever truncated at that width.
; CHECK-LABEL: define i32 @dispatch(
; CHECK: %[[IC:.+]] = call i32 %fn(ptr %obj)
; CHECK-NEXT: ret i32 %[[IC]]
define i32 @dispatch(ptr %obj, ptr %fn) {
  %bits = ptrtoint ptr %obj to i64
  %ic = call i64 %fn(i64 %bits)
  %t = trunc i64 %ic to i32
  ret i32 %t
}

; A stack slot is one variable: an address stored to it is an address when
; it is loaded back, so the parameter stored there is a pointer.
; CHECK-LABEL: define void @slotted(ptr
define void @slotted(i64 %p) {
  %slot = alloca i64
  store i64 %p, ptr %slot
  %v = load i64, ptr %slot
  %q = inttoptr i64 %v to ptr
  store i8 0, ptr %q
  ret void
}

; A constant is one value for the whole module: an `i64 11` used as an
; address in one function is no evidence about another that returns 11.
; CHECK-LABEL: define i64 @eleven(
; CHECK: ret i64 11
define i64 @eleven() {
  ret i64 11
}
define void @poke_eleven(ptr %q) {
  %p = inttoptr i64 11 to ptr
  store i8 0, ptr %p
  %r = call i64 @eleven()
  store i64 %r, ptr %q
  ret void
}

; A pointer stored to a stack slot and read back as an integer is still the
; caller's pointer: the parameter it is passed to is a pointer.
; CHECK-LABEL: define void @takes_held(ptr
define void @takes_held(i64 %p) {
  %c = call i64 @sink(i64 %p)
  ret void
}
define void @held(ptr %p) {
  %slot = alloca i64
  store ptr %p, ptr %slot
  %v = load i64, ptr %slot
  call void @takes_held(i64 %v)
  ret void
}

; An unoptimized function keeps no value in a register across blocks, so a
; register merged at its return is what each path last left in rax (here
; `this`, loaded to address a field, or a callee's result): void. The same
; shape in optimized code may be a real result and stays.
; CHECK-LABEL: define void @o0_merge(
; CHECK-LABEL: define i64 @opt_merge(
define i64 @o0_merge(ptr %this) "revenant.o0" {
entry:
  %bits = ptrtoint ptr %this to i64
  %v = load i64, ptr %this
  %c = icmp eq i64 %v, 0
  br i1 %c, label %end, label %more
more:
  %r = call i64 @sink(i64 %v)
  br label %end
end:
  %p = phi i64 [ %bits, %entry ], [ %r, %more ]
  ret i64 %p
}
define i64 @opt_merge(ptr %this) {
entry:
  %bits = ptrtoint ptr %this to i64
  %v = load i64, ptr %this
  %c = icmp eq i64 %v, 0
  br i1 %c, label %end, label %more
more:
  %r = call i64 @sink(i64 %v)
  br label %end
end:
  %p = phi i64 [ %bits, %entry ], [ %r, %more ]
  ret i64 %p
}
define void @merge_callers(ptr %this) {
  %a = call i64 @o0_merge(ptr %this)
  %b = call i64 @opt_merge(ptr %this)
  ret void
}

; A byte result that only ever holds 0 or 1 is a bool: unoptimized code
; masks the bool it returns (`and al, 1`). A byte read from memory is a
; char and stays one.
; CHECK-LABEL: define i1 @is_set(
; CHECK: ret i1
define i8 @is_set(ptr %p) {
  %v = load i8, ptr %p
  %b = and i8 %v, 1
  ret i8 %b
}
; CHECK-LABEL: define i1 @not_set(
; CHECK: call i1 @is_set(
; CHECK: ret i1
define i8 @not_set(ptr %p) {
  %r = call i8 @is_set(ptr %p)
  %m = and i8 %r, 1
  %x = xor i8 %m, 1
  ret i8 %x
}
; CHECK-LABEL: define i8 @get_char(
; CHECK: ret i8 %v
define i8 @get_char(ptr %p) {
  %v = load i8, ptr %p
  ret i8 %v
}
define void @use_bytes(ptr %p, ptr %q) {
  %r = call i8 @not_set(ptr %p)
  store i8 %r, ptr %q
  %c = call i8 @get_char(ptr %p)
  store i8 %c, ptr %q
  ret void
}
