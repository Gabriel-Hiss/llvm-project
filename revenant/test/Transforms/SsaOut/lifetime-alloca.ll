; Reduced from fmt-cl-o2gl's sub_1400761F0: an inlined frame alloca is
; referenced by both the lifetime intrinsic and ordinary pointer operations.
; RUN: %revenant-opt -passes=function(revenant-ssaout),verify -S %s | %FileCheck %s
;
; CHECK-LABEL: define i64 @sub_1400761F0(
; CHECK: %v_frame.i = alloca ptr
; CHECK: %frame.i = alloca [1336 x i8]
; CHECK: store ptr %frame.i, ptr %v_frame.i
; CHECK: call void @llvm.lifetime.start.p0(ptr nonnull %frame.i)
; CHECK: load ptr, ptr %v_frame.i
; CHECK: call void @llvm.lifetime.end.p0(ptr nonnull %frame.i)

declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.lifetime.end.p0(ptr captures(none))

define i64 @sub_1400761F0() {
entry:
  %frame.i = alloca [1336 x i8], align 1
  call void @llvm.lifetime.start.p0(ptr nonnull %frame.i)
  %fp31.i = getelementptr inbounds i8, ptr %frame.i, i64 8
  %rsp32.i = ptrtoint ptr %fp31.i to i64
  call void @llvm.lifetime.end.p0(ptr nonnull %frame.i)
  ret i64 %rsp32.i
}
