; The MSVC /GS stack cookie: the prologue xors the cookie with the
; stack pointer and stores the result in the frame, the epilogue reloads
; it, xors it with the stack pointer again and compares it with the
; cookie. A mismatch calls __security_check_cookie, which aborts. The
; compiler's own pre-optimization IR has none of it: the backend inserts
; the cookie after the frontend produced the IR. This pass removes the
; sequence and leaves the marker the source emitter reads.
; RUN: %revenant-opt -passes=revenant-gscookie -S %s | %FileCheck %s

declare void @__security_check_cookie(i64)
@__security_cookie = internal global i64 47936899621426

define i32 @guarded(ptr %frame) {
; CHECK-LABEL: define i32 @guarded(
; CHECK-NOT: xor
; CHECK-NOT: icmp
; CHECK-NOT: __security_check_cookie
; CHECK: br label %ok
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
