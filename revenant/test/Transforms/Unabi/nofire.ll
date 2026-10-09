; No-fire guards: each annotated function below carries an annotation the
; pass must refuse (attribute kept, body untouched). Covers: arity mismatch,
; unknown token, sret slot that escapes via load, ctor shape broken (returns
; an unrelated value), musttail call site, indirect use (address taken), and
; pre-existing sret/byval attributes.
; RUN: %revenant-opt -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.S = type { i64 }

; Arity mismatch: 1 token, 2 params.
; CHECK-LABEL: define i64 @arity(i64 %0, i64 %1)
; CHECK-SAME: #0
define i64 @arity(i64 %0, i64 %1) #0 {
entry:
  ret i64 %0
}

; Unknown token.
; CHECK-LABEL: define i64 @badtoken(i64 %0)
; CHECK-SAME: #1
define i64 @badtoken(i64 %0) #1 {
entry:
  ret i64 %0
}

; Sret slot escapes through a load: not store-only.
; CHECK-LABEL: define void @escaper(ptr %0, ptr %1)
; CHECK-SAME: #2
define void @escaper(ptr %0, ptr %1) #2 {
entry:
  %v = load i64, ptr %1, align 8, !revenant.va !{i64 9000}
  store i64 %v, ptr %0, align 8, !revenant.va !{i64 9004}
  ret void
}

; Broken ctor: returns an unrelated constant, not param 0.
; CHECK-LABEL: define ptr @notctor(ptr %0)
; CHECK-SAME: #3
define ptr @notctor(ptr %0) #3 {
entry:
  ret ptr null
}

; musttail call site: signature rewrite would break the tail guarantee.
; CHECK: define i64 @callee_t(i64 %0) #0
define i64 @callee_t(i64 %0) #4 {
entry:
  ret i64 %0
}

define i64 @tailed(i64 %0) {
entry:
  %r = musttail call i64 @callee_t(i64 %0)
  ret i64 %r
}

; Address taken: stored to a global, call sites unknowable.
; CHECK: define i64 @addrTaken(i64 %0) #0
@slot = global ptr null
define i64 @addrTaken(i64 %0) #5 {
entry:
  store ptr @addrTaken, ptr @slot, align 8
  ret i64 %0
}

; Pre-existing sret: owned by someone else.
; CHECK: define void @hasSret(ptr sret(%struct.S) %0) #4
define void @hasSret(ptr sret(%struct.S) %0) #6 {
entry:
  ret void
}

attributes #0 = { "revenant.sig"="r32 => r32" }
attributes #1 = { "revenant.sig"="bogus => r64" }
attributes #2 = { "revenant.sig"="p S%struct.S => S%struct.S" }
attributes #3 = { "revenant.sig"="p => t" }
attributes #4 = { "revenant.sig"="r32 => r32" }
attributes #5 = { "revenant.sig"="r32 => r32" }
attributes #6 = { "revenant.sig"="p => v" }
