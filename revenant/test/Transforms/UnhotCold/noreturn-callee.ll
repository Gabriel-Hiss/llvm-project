; RUN: %revenant-opt -passes=revenant-unhotcold -S %s | %FileCheck %s
;
; The noreturn contract lives on the callee, but the call site returns
; into live code (`ret` follows the call): the merge must decline BEFORE
; moving any block, leaving the donor intact and the module verifiable.
; Before the fix the blocks moved first and the bail left an orphan block
; plus an emptied donor (verifier abort with leaked !revenant.va).

declare void @abort() noreturn

define internal void @hot() {
; CHECK-LABEL: define internal void @hot(
entry:
  call void @cold(), !revenant.va !{i64 4096}
; CHECK: call void @cold()
  ret void, !revenant.va !{i64 4100}
}

; CHECK-LABEL: define internal void @cold(
define internal void @cold() noreturn {
entry:
  call void @abort() noreturn, !revenant.va !{i64 1048576}
  unreachable, !revenant.va !{i64 1048580}
}
