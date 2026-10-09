; End-to-end: real corpus shapes, lowered the way the lifter emits them,
; annotated with revenant.sig, run through the pass, checked against ir-pre.
; Corpus: audio-nodegraph-clang-o1 node_graph.pre.ll
;  - ?indexOf@NodeGraph: (ptr this, ptr sret-optional, i32) -> void
;    becomes %struct.Optional return (struct named as in ir-pre).
;  - ??0GraphNode ctor: (ptr) -> ptr returned becomes void.
;  - ?addNode: (ptr this, i32, float, float) -> i32, lifted widths widened
;    to i64/f64, narrowed back.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.Optional = type { i64, i8 }
%struct.GraphNode = type { i32, i32, float, float }

; Lifted sret: hidden slot is a plain ptr param, void return.
; CHECK-LABEL: define %struct.Optional @indexOf(ptr %this, i32 %a1)
; CHECK: %sret.ret = load %struct.Optional, ptr %sret.slot
; CHECK: ret %struct.Optional %sret.ret
define void @indexOf(ptr %0, ptr %1, i64 %2) #0 {
entry:
  store i64 7, ptr %1, align 8, !revenant.va !{i64 5000}
  %f = getelementptr inbounds nuw %struct.Optional, ptr %1, i32 0, i32 1, !revenant.va !{i64 5008}
  store i8 1, ptr %f, align 8, !revenant.va !{i64 5008}
  ret void
}

; Lifted ctor: returns its receiver in rax.
; CHECK-LABEL: define void @ctorGraphNode(ptr %a0)
define ptr @ctorGraphNode(ptr %0) #1 {
entry:
  %g0 = getelementptr inbounds nuw %struct.GraphNode, ptr %0, i32 0, i32 0, !revenant.va !{i64 5100}
  store i32 0, ptr %g0, align 8, !revenant.va !{i64 5100}
  ret ptr %0
}

; Lifted widths: i32->i64, float->f64.
; CHECK-LABEL: define i32 @addNode(ptr %this, i32 %a1, float %a2, float %a3)
; CHECK: ret i32
define i64 @addNode(i64 %0, i64 %1, double %2, double %3) #2 {
entry:
  %t1 = trunc i64 %1 to i32, !revenant.va !{i64 5200}
  %t2 = fptrunc double %2 to float, !revenant.va !{i64 5204}
  %t3 = fptrunc double %3 to float, !revenant.va !{i64 5208}
  %m = mul i32 %t1, 3, !revenant.va !{i64 5212}
  %e = zext i32 %m to i64, !revenant.va !{i64 5216}
  ret i64 %e
}

attributes #0 = { "revenant.sig"="T S%struct.Optional r32 => S%struct.Optional" }
attributes #1 = { "revenant.sig"="p => t" }
attributes #2 = { "revenant.sig"="T r32 f32 f32 => r32" }
