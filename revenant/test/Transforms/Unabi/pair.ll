; RAX-coerced small struct back to a struct return: clang coerces small
; aggregates to a single integer in RAX (Win64). The annotation names the
; source struct; the width check (store size == old return width) guards it.
; Corpus: audio-nodegraph-clang-o1 ?inputPortPos (PortPos{float,float} -> i64).
; RUN: %revenant-opt -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.PortPos = type { float, float }

; CHECK-LABEL: define %struct.PortPos @portPos(ptr %a0)
; CHECK: %pair.ret = load %struct.PortPos, ptr %pair.tmp
define i64 @portPos(ptr %0) #0 {
entry:
  %x = load float, ptr %0, align 4, !revenant.va !{i64 6000}
  %t = alloca %struct.PortPos, align 4
  %g0 = getelementptr inbounds nuw %struct.PortPos, ptr %t, i32 0, i32 0, !revenant.va !{i64 6016}
  store float %x, ptr %g0, align 4, !revenant.va !{i64 6016}
  %c = load i64, ptr %t, align 4, !revenant.va !{i64 6024}
  ret i64 %c
}

attributes #0 = { "revenant.sig"="p => P%struct.PortPos" }
