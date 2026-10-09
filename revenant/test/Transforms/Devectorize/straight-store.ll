; A 128-bit vector store splits into per-lane scalar stores covering the
; same bytes, before the observing call. Ports
; straight_store_stays_before_observer_call.
; RUN: %revenant-opt -passes=revenant-devectorize -S %s | %FileCheck %s

declare void @observe(ptr)

define void @slp_store(ptr %p) {
entry:
  store <4 x i32> zeroinitializer, ptr %p, align 16
  call void @observe(ptr %p)
  ret void
}

; Four scalar stores precede the call, covering 16 bytes.
; CHECK-COUNT-4: store i32 0, ptr {{.*}}
; CHECK: call void @observe
; CHECK-NOT: store <4 x i32>
