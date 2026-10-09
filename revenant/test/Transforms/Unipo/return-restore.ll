; Return elimination undo: errfile shape (lua-clang-o1/lauxlib i32 -> void).
; The post body ends `tail call void @lua_settop; ret void`; pre ended
; `call void @lua_settop; ret i32 6`. The dead computed value (here the
; surviving call result %v, in the binary the eax move) is restored.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unipo -verify-each -S %s | %FileCheck %s

; CHECK-LABEL: define i32 @errfile_like(ptr %a, ptr %b, i32 %n)
; CHECK: ret i32 %v
define void @errfile_like(ptr %a, ptr %b, i32 %n) {
entry:
  %v = call i32 @produces(ptr %a, ptr %b, i32 %n), !revenant.va !{i64 7000}
  call void @consume(ptr %a, i32 %n), !revenant.va !{i64 7008}
  ret void
}

declare i32 @produces(ptr, ptr, i32)
declare void @consume(ptr, i32)

; No-fire: mixed void/value returns are not an elimination shape.
; CHECK-LABEL: define void @mixed(
; CHECK: ret void
define void @mixed(ptr %a, i1 %c) {
entry:
  br i1 %c, label %t, label %f
t:
  call void @consume(ptr %a, i32 1)
  ret void
f:
  call void @consume(ptr %a, i32 2)
  ret void
}
