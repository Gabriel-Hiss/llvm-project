; No personality, no EH: a plain function is untouched (never invent EH).
; RUN: %revenant-opt -passes=revenant-uneh -S %s | %FileCheck %s

declare void @dtor(ptr)

define i64 @plain(ptr %in) {
entry:
  ; CHECK-LABEL: define i64 @plain(
  ; CHECK: call void @dtor(ptr %in)
  ; CHECK-NOT: cleanuppad
  ; CHECK-NOT: invoke
  ; CHECK: ret i64 0
  call void @dtor(ptr %in), !revenant.va !0
  ret i64 0, !revenant.va !0
}

!0 = !{i64 4198400}
