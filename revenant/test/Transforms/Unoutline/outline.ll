; MachineOutliner-like shared fragment and call thunk, modeled on the
; OUTLINED_FUNCTION call pattern emitted by clang -Oz (tested in the
; Swe2DeoptOutliner probe from 2026-10-09). No symbol names drive matching.
; RUN: %opt -load-pass-plugin=%plugin -passes='revenant-unoutline,verify' -S %s | %FileCheck %s

define i32 @outlined_fragment(i32 %x) {
entry:
  %sum = add i32 %x, 37, !revenant.va !0
  ret i32 %sum
}

define i32 @outlined_thunk(i32 %x) {
entry:
  %r = call i32 @target(i32 %x), !revenant.va !0
  ret i32 %r
}

declare i32 @target(i32)

define i32 @first(i32 %x) {
entry:
  %r = call i32 @outlined_fragment(i32 %x)
  ret i32 %r
}

define i32 @second(i32 %x) {
entry:
  %r = call i32 @outlined_fragment(i32 %x)
  ret i32 %r
}

define i32 @third(i32 %x) {
entry:
  %r = call i32 @outlined_thunk(i32 %x)
  ret i32 %r
}

define i32 @fourth(i32 %x) {
entry:
  %r = call i32 @outlined_thunk(i32 %x)
  ret i32 %r
}

define i32 @ordinary_helper(i32 %x) {
entry:
  %sum = add i32 %x, 1
  ret i32 %sum
}

define i32 @ordinary_user(i32 %x) {
entry:
  %r = call i32 @ordinary_helper(i32 %x)
  ret i32 %r
}

; CHECK-NOT: call i32 @outlined_fragment
; CHECK-LABEL: define i32 @first
; CHECK: add i32 %x, 37, !revenant.va !
; CHECK-LABEL: define i32 @second
; CHECK: add i32 %x, 37, !revenant.va !
; CHECK-NOT: call i32 @outlined_thunk
; CHECK-LABEL: define i32 @third
; CHECK: call i32 @target(i32 %x)
; CHECK-LABEL: define i32 @fourth
; CHECK: call i32 @target(i32 %x)
; CHECK-LABEL: define i32 @ordinary_user
; CHECK: call i32 @ordinary_helper(i32 %x)

!0 = !{i64 4096}
