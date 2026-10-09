; sret slot written by llvm.memcpy (not just plain stores): the common
; lowering for struct copies in ir-pre (e.g. vector-iterator arithmetic in
; audio-nodegraph-clang-o1 ??H?$_Vector_iterator). A memcpy that READS the
; slot (slot as source) still bails.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.It = type { ptr }

; CHECK-LABEL: define %struct.It @adv(ptr %a0)
; CHECK: call void @llvm.memcpy.p0.p0.i64(ptr %sret.slot, ptr %a0, i64 8, i1 false)
; CHECK: ret %struct.It %sret.ret
define void @adv(ptr %0, ptr %1) #0 {
entry:
  %u = alloca %struct.It, align 8
  call void @llvm.memcpy.p0.p0.i64(ptr %1, ptr %0, i64 8, i1 false)
  ret void
}
declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)

; Slot as the copy source: reads the slot, not an sret shape.
; CHECK-LABEL: define void @reader(ptr %0, ptr %1)
; CHECK-SAME: #1
define void @reader(ptr %0, ptr %1) #1 {
entry:
  call void @llvm.memcpy.p0.p0.i64(ptr %0, ptr %1, i64 8, i1 false)
  ret void
}

attributes #0 = { "revenant.sig"="p S%struct.It => S%struct.It" }
attributes #1 = { "revenant.sig"="p S%struct.It => S%struct.It" }
