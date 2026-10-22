; The rustc MIR block shape: a call that ends a MIR block gets a block
; of its own, so the code that follows it starts a new block. The
; optimizer merges those blocks (SimplifyCFG folds the `br label` chain
; into the block that follows), and this pass puts them back.
; RUN: %revenant-opt -passes=revenant-rustblocks -S %s | %FileCheck %s

!llvm.module.flags = !{!0}
!0 = !{i32 4, !"revenant.lang", !"rust"}

declare void @drop_it()
declare i64 @value_of(i64)

; Calls whose result is dropped end the MIR block: the code that follows
; them moves into a block of its own.
define void @ends_block(i64 %0) {
; CHECK-LABEL: define void @ends_block(
entry:
; CHECK: call void @drop_it()
; CHECK: br label %bb_x0
; CHECK: bb_x0:
; CHECK: call void @drop_it()
  call void @drop_it()
  %1 = add i64 %0, 1
  call void @drop_it()
  %2 = add i64 %1, 1
  call void @drop_it()
  br label %done
done:
  ret void
}

; A call whose result is used is a statement inside the block: the block
; does not end there, so nothing is split.
define i64 @keeps_block(i64 %0) {
; CHECK-LABEL: define i64 @keeps_block(
; CHECK-NOT: br label %bb_x
entry:
  %1 = call i64 @value_of(i64 %0)
  %2 = add i64 %1, 1
  ret i64 %2
}

; A call that is followed by the block's `unreachable` already ends its
; block: that is how rustc spells a diverging call.
define void @diverges() {
; CHECK-LABEL: define void @diverges(
; CHECK-NOT: br label %bb_x
entry:
  call void @drop_it()
  unreachable
}

; A function with a single dropped-result call has at most one boundary
; the compiler's IR could have kept apart, so the pass leaves it alone.
define void @one_call(i64 %0) {
; CHECK-LABEL: define void @one_call(
; CHECK-NOT: br label %bb_x
entry:
  call void @drop_it()
  %1 = add i64 %0, 1
  br label %done
done:
  ret void
}
