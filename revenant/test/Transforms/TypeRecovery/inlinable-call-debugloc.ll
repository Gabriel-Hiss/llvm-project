; RUN: revenant-opt -passes=revenant-types -S %s | FileCheck %s
;
; Type recovery adds a location before replay can inline a generated call
; whose lifted IR had no !dbg.

; CHECK-LABEL: define i32 @sub_140004000(
; CHECK: %result = call i32 @helper(i32 %a0), !dbg ![[CALL_LOC:[0-9]+]], !revenant.va !{{[0-9]+}}
; CHECK: ![[SP:[0-9]+]] = distinct !DISubprogram(name: "sub_140004000"
; CHECK: ![[CALL_LOC]] = !DILocation(line: 1, column: 1, scope: ![[SP]])

define i32 @sub_140004000(i32 %a0) !revenant.va !0 {
entry:
  %result = call i32 @helper(i32 %a0), !revenant.va !1
  ret i32 %result, !revenant.va !2
}

define internal i32 @helper(i32 %value) #0 {
entry:
  %result = add i32 %value, 1
  ret i32 %result
}

attributes #0 = { alwaysinline }

!0 = !{i64 140004000}
!1 = !{i64 140004003}
!2 = !{i64 140004008}
