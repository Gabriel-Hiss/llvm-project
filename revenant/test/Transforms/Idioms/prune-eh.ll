; RUN: %revenant-opt -passes='revenant-prune-eh,verify' -S %s | %FileCheck %s
; An invoke of a function that cannot unwind becomes a call, and the cleanup
; only it reached goes. A callee that may unwind keeps its invoke, as do the
; invokes of an unoptimized function.
target datalayout = "e-p:64:64-i64:64-n8:16:32:64"

declare i32 @__CxxFrameHandler3(...)
declare void @_CxxThrowException(ptr, ptr)
declare i64 @CloseHandle(i64)
declare void @drop(ptr)

; Calls nothing that unwinds: itself through @quiet_too, and a C import.
define void @quiet(ptr %p) {
  store i8 0, ptr %p
  call void @quiet_too(ptr %p)
  %h = call i64 @CloseHandle(i64 0)
  ret void
}
define void @quiet_too(ptr %p) {
  call void @quiet(ptr %p)
  ret void
}
; Raises an exception.
define void @loud() {
  call void @_CxxThrowException(ptr null, ptr null)
  ret void
}

; CHECK-LABEL: define void @caller(
; CHECK: call void @quiet(ptr %p)
; CHECK-NOT: invoke void @quiet
; CHECK: invoke void @loud()
; CHECK: ret void
; CHECK: %c2 = cleanuppad
; CHECK-NOT: cleanuppad
define void @caller(ptr %p) personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @quiet(ptr %p) to label %next unwind label %pad1
next:
  invoke void @loud() to label %done unwind label %pad2
done:
  ret void
pad1:
  %c1 = cleanuppad within none []
  call void @drop(ptr %p) [ "funclet"(token %c1) ]
  cleanupret from %c1 unwind to caller
pad2:
  %c2 = cleanuppad within none []
  call void @drop(ptr %p) [ "funclet"(token %c2) ]
  cleanupret from %c2 unwind to caller
}

; CHECK-LABEL: define void @unoptimized(
; CHECK: invoke void @quiet(
define void @unoptimized(ptr %p) "revenant.o0" personality ptr @__CxxFrameHandler3 {
entry:
  invoke void @quiet(ptr %p) to label %done unwind label %pad
done:
  ret void
pad:
  %c = cleanuppad within none []
  cleanupret from %c unwind to caller
}
