; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unhotcold -S %s | %FileCheck %s

; GCC .cold merge: lua-gcc-o2 `luaD_throw` -> `luaD_throw.cold`.
; Hot block at VA 0x14000dab0 calls the cold chunk at 0x140038a50
; (gap ~178 KiB); the cold chunk is a noreturn trap stub (`call abort`
; with noreturn attr, then unreachable). After the pass the cold block
; lives in the hot function and the .cold function is gone.

declare void @abort() noreturn

define internal void @luaD_throw.cold() noreturn {
; CHECK-NOT: define {{.*}} @luaD_throw.cold(
cold.only:
  call void @abort(), !revenant.va !{i64 5368920144}
  unreachable
}

define void @luaD_throw(i32 %err) {
; CHECK-LABEL: define void @luaD_throw(
entry:
  %hot0 = add i32 %err, 1, !revenant.va !{i64 5368741552}
  %c = icmp eq i32 %hot0, 0, !revenant.va !{i64 5368741568}
  br i1 %c, label %ret, label %dispatch, !revenant.va !{i64 5368741576}

dispatch:
  call void @luaD_throw.cold(), !revenant.va !{i64 5368741592}
  unreachable

ret:
  ret void
; CHECK: dispatch:
; CHECK: call void @abort()
; CHECK: unreachable
}
