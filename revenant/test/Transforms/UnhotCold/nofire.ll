; RUN: %revenant-opt -passes=revenant-unhotcold -S %s | %FileCheck %s

; Negative tests: the pass must not fire here.
;
; 1. @two_callers: cold-like body but two hot callers (not single-caller).
; 2. @near_chunk: cold-like body but VA-near the hot caller (same locality,
;    not a far split chunk).
; 3. @store_chunk: far away but stores to memory (not a trap stub; could be
;    a real outlined function with effects we cannot prove dead).
; 4. @balanced: branch weights are equal, no inversion to undo.

declare void @abort() noreturn

define internal void @shared_cold() noreturn {
cold.only:
  call void @abort(), !revenant.va !{i64 5368920144}
  unreachable
}

define void @two_callers(i32 %n) {
; CHECK-LABEL: define void @two_callers(
entry:
  %c = icmp eq i32 %n, 0, !revenant.va !{i64 5368741552}
  br i1 %c, label %a, label %b, !revenant.va !{i64 5368741556}

a:
  call void @shared_cold(), !revenant.va !{i64 5368741560}
  unreachable

b:
  call void @shared_cold(), !revenant.va !{i64 5368741564}
  unreachable
; CHECK: call void @shared_cold()
}

define internal void @near.cold() noreturn {
cold.only:
  call void @abort(), !revenant.va !{i64 5368741900}
  unreachable
}

define void @near_chunk(i32 %n) {
; CHECK-LABEL: define void @near_chunk(
entry:
  %c = icmp eq i32 %n, 0, !revenant.va !{i64 5368741552}
  br i1 %c, label %ret, label %dispatch, !revenant.va !{i64 5368741556}

dispatch:
  call void @near.cold(), !revenant.va !{i64 5368741560}
  unreachable

ret:
  ret void
; CHECK: call void @near.cold()
}

define internal void @storing.cold() {
cold.only:
  store i32 0, ptr null, !revenant.va !{i64 5368920144}
  ret void
}

define void @store_chunk(i32 %n) {
; CHECK-LABEL: define void @store_chunk(
entry:
  %c = icmp eq i32 %n, 0, !revenant.va !{i64 5368741552}
  br i1 %c, label %ret, label %dispatch, !revenant.va !{i64 5368741556}

dispatch:
  call void @storing.cold(), !revenant.va !{i64 5368741560}
  unreachable

ret:
  ret void
; CHECK: call void @storing.cold()
}

define void @balanced(i1 %x) {
; CHECK-LABEL: define void @balanced(
entry:
  br i1 %x, label %cold, label %hot, !prof !{!"branch_weights", i32 50, i32 50}, !revenant.va !{i64 4096}

hot:
  ret void

cold:
  ret void
; CHECK: br i1 %x, label %cold, label %hot
}
