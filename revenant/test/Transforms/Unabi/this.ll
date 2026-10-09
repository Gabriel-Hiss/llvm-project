; `this` for C++ methods: the lifted form passes the receiver as a plain
; i64 with no name; the annotation restores the ptr receiver named `this`.
; Corpus: cpp-clang-o2 ?area@Rect (ptr this -> this), ?area@Circle.
; RUN: %opt -load-pass-plugin=%plugin -passes=revenant-unabi -verify-each -S %s | %FileCheck %s

%struct.Rect = type { ptr, i32, i32, i32 }

; CHECK-LABEL: define i32 @area(ptr %this)
define i32 @area(i64 %0) #0 {
entry:
  %p = inttoptr i64 %0 to ptr, !revenant.va !{i64 7000}
  %w = load i32, ptr %p, align 4, !revenant.va !{i64 7008}
  ret i32 %w
}

attributes #0 = { "revenant.sig"="T => r32" }
