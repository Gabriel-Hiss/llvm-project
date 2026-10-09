; sret back to struct return: the lifted form is void with a hidden slot
; param (Win64 passes the slot in rdx for methods, rcx for free functions;
; the rewrite is positional). Corpus: audio-nodegraph-clang-o1
; ?indexOf@NodeGraph (ptr this, ptr sret, i32) -> optional, and cpp-clang-o2
; ?begin/?end vector iterators with the same shape.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.Optional = type { i64, i8 }
; CHECK-LABEL: define %struct.Optional @indexOf(ptr %this, i32 %a1)
; CHECK: %sret.ret = load %struct.Optional, ptr %sret.slot
; CHECK: ret %struct.Optional %sret.ret
define void @indexOf(ptr %0, ptr %1, i32 %2) #0 {
  store i64 7, ptr %1, align 8, !revenant.va !{i64 5000}
  %f = getelementptr inbounds nuw %struct.Optional, ptr %1, i32 0, i32 1, !revenant.va !{i64 5008}
  store i8 1, ptr %f, align 8, !revenant.va !{i64 5008}
  ret void
}

; The call site keeps its slot: the returned struct is stored back.
; CHECK-LABEL: define void @caller(ptr %this, i32 %id)
; CHECK: call %struct.Optional @indexOf(ptr %this, i32 %id)
; CHECK: store %struct.Optional %0, ptr %slot
define void @caller(ptr %this, i32 %id) {
entry:
  %slot = alloca %struct.Optional, align 8
  call void @indexOf(ptr %this, ptr %slot, i32 %id)
  ret void
}

attributes #0 = { "revenant.sig"="T S%struct.Optional r32 => S%struct.Optional" }
