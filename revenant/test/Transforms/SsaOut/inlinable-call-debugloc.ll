; RUN: revenant-opt -passes=function(revenant-ssaout) -S %s | FileCheck %s
; RUN: revenant-opt -passes=function(revenant-ssaout),always-inline,verify -disable-output %s
;
; SSA-out creates the first DISubprogram when revenant-types is disabled.
; Calls must still have locations so the inliner can verify the module.

; CHECK-LABEL: define i32 @sub_140004000(
; CHECK: %result = call i32 @helper(i32 %a0), !dbg ![[CALL_LOC:[0-9]+]], !revenant.va !{{[0-9]+}}
; CHECK: %result2 = call i32 @helper(i32 %result), !dbg ![[NO_VA_LOC:[0-9]+]]{{$}}
; CHECK: ![[SP:[0-9]+]] = distinct !DISubprogram(name: "sub_140004000"
; CHECK: ![[CALL_LOC]] = !DILocation(line: 140004003, scope: ![[SP]])
; CHECK: ![[NO_VA_LOC]] = !DILocation(line: 0, scope: ![[SP]])

define i32 @sub_140004000(i32 %a0) !revenant.va !0 {
entry:
  %result = call i32 @helper(i32 %a0), !revenant.va !1
  %result2 = call i32 @helper(i32 %result)
  ret i32 %result2, !revenant.va !2
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
