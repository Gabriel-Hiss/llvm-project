; The MSVC /GS stack cookie: the prologue xors the cookie with the
; stack pointer and stores the result in the frame, the epilogue reloads
; it, xors it with the stack pointer again and compares it with the
; cookie. A mismatch calls __security_check_cookie, which aborts. The
; compiler's own pre-optimization IR has none of it: the backend inserts
; the cookie after the frontend produced the IR. This pass removes the
; sequence, the failure block and the split the check made, and leaves
; the marker the source emitter reads.
; RUN: %revenant-opt -passes=revenant-gscookie -S %s | %FileCheck %s

declare void @__security_check_cookie(i64)
@__security_cookie = internal global i64 47936899621426

define i32 @guarded(ptr %frame) {
; CHECK-LABEL: define i32 @guarded(
; CHECK-NOT: xor
; CHECK-NOT: icmp
; CHECK-NOT: __security_check_cookie
; CHECK-NOT: br
; CHECK-NOT: unreachable
; CHECK: ret i32 1
; CHECK-NEXT: }
entry:
  %fp = getelementptr inbounds i8, ptr %frame, i64 8
  %rsp = ptrtoint ptr %fp to i64
  %cookie = load i64, ptr @__security_cookie
  %setup = xor i64 %cookie, %rsp
  %slot = getelementptr inbounds i64, ptr %frame, i64 16
  store i64 %setup, ptr %slot
  %stored = load i64, ptr %slot
  %computed = xor i64 %stored, %rsp
  %again = load i64, ptr @__security_cookie
  %eq = icmp eq i64 %again, %computed
  br i1 %eq, label %ok, label %fail
ok:
  ret i32 1
fail:
  call void @__security_check_cookie(i64 %computed)
  unreachable
}

; An unoptimized frame keeps the cookie in its own slot and reloads it on
; the failure edge, so the check call gets a second xor of the stack
; pointer. The sequence goes all the same, and so does the slot that only
; held the cookie.
define i32 @reloaded() {
; CHECK-LABEL: define i32 @reloaded(
; CHECK-NOT: %cookie.slot
; CHECK-NOT: xor
; CHECK-NOT: __security_check_cookie
; CHECK-NOT: br
; CHECK: ret i32 1
; CHECK-NEXT: }
entry:
  %cookie.slot = alloca i64
  %frame = alloca [32 x i8]
  %rsp = ptrtoint ptr %frame to i64
  %cookie = load i64, ptr @__security_cookie
  %setup = xor i64 %cookie, %rsp
  store i64 %setup, ptr %cookie.slot
  %stored = load i64, ptr %cookie.slot
  %computed = xor i64 %stored, %rsp
  %again = load i64, ptr @__security_cookie
  %eq = icmp eq i64 %again, %computed
  br i1 %eq, label %ok3, label %fail3
ok3:
  ret i32 1
fail3:
  %reload = load i64, ptr %cookie.slot
  %check = xor i64 %reload, %rsp
  call void @__security_check_cookie(i64 %check)
  br label %ok3
}

; The cleanup folded the failure edge away (the check does not return when
; it fails), so only the subtraction is left, in the return register. It
; goes, and so does the prologue store.
define i64 @folded() {
; CHECK-LABEL: define i64 @folded(
; CHECK-NOT: xor
; CHECK-NOT: sub
; CHECK-NOT: store
; CHECK: ret i64 undef
entry:
  %cookie.slot = alloca i64
  %frame = alloca [32 x i8]
  %rsp = ptrtoint ptr %frame to i64
  %cookie = load i64, ptr @__security_cookie
  %setup = xor i64 %cookie, %rsp
  store i64 %setup, ptr %cookie.slot
  %stored = load i64, ptr %cookie.slot
  %computed = xor i64 %stored, %rsp
  %again = load i64, ptr @__security_cookie
  %diff = sub i64 %again, %computed
  ret i64 %diff
}

; A function without the cookie shape is left alone.
define i32 @plain(ptr %frame) {
; CHECK-LABEL: define i32 @plain(
; CHECK: icmp eq i64 %a, %b
; CHECK: call void @__security_check_cookie(i64 %a)
entry:
  %a = load i64, ptr %frame
  %b = load i64, ptr @__security_cookie
  %eq = icmp eq i64 %a, %b
  br i1 %eq, label %ok2, label %fail2
ok2:
  ret i32 1
fail2:
  call void @__security_check_cookie(i64 %a)
  unreachable
}

; The marker: the function that had the cookie carries it, so the source
; emitter can tell that the cookie was removed.
; CHECK: "rv.gscookie"="1"
