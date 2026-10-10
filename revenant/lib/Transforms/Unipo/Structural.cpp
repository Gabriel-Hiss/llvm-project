// Structural clone folding is disabled. Three review wrong-codes showed
// that no body comparison can prove two entry points came from one source
// function: distinct `sub_0x` declarations linked to different behavior
// (trap-alias: 0 -> 1), opaque observers recording different addresses
// (external-observer: 0 -> 1), distinct exit codes behind dead ends
// (trap-side-effects: 2 -> 1), and volatile loads from different objects
// (volatile-observer). Identical bodies are not proof of shared source
// identity either. This file now provides only strict constant identity
// for the signature-restoration rewrites in Unipo.cpp, which rebind the
// same constant object to a restored formal.
#include "llvm/IR/Constants.h"

using namespace llvm;

namespace revenant {
// Strict identity: two constants are equivalent only when they are the
// same object. Two different globals are always different, even with
// identical contents and sections; two different declarations are always
// different, whatever their attributes.
bool constantsEquivalent(const Constant *A, const Constant *B) {
  return A != nullptr && A == B;
}

// The structural fold is removed: no body comparison can prove shared
// source identity (see file header). Preserved as a no-op so the
// pipeline name keeps working; always reports no change.
bool foldStructuralIPO(Module &) { return false; }
} // namespace revenant
