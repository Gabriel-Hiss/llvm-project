//===- revenant/Transforms/Uncxx/Uncxx.h - inline STL recovery -*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
// Recovers inlined C++ standard-library operations back to calls. At -O1+
// the compiler pastes small STL helpers (vector::size/data/operator[]/
// push_back fast path, basic_string SSO decode/append fast path/_Tidy,
// shared_ptr/unique_ptr refcount and dispose bodies, map iterator node
// hops) directly into the caller, so lifted IR shows raw field loads and
// pointer bumps instead of the member call the source had. This pass
// recognizes those shapes and rewrites them to `revenant.cxx.*` helper
// calls that carry the member name and keep exact semantics.
//
// Complements revenant-patterns (owner: the patterns pass): patterns needs
// an oracle catalog or a compiled snippet library and rewrites contiguous
// single-block regions; this pass is table-free, handles multi-block
// idioms (capacity guard + growth call diamonds, CAS refcount loops,
// dtor free sequences), and knows the per-STL field layout.
//
// Flavor gating: the layout differs per STL implementation (MSVC STL
// vector/string share the 3-pointer layout, libstdc++ string keeps its
// pointer field with an inline buffer union, libc++ uses the packed-bit
// layout). The engine may provide the STL flavor detected by CompilerProfile;
// `Auto` falls back to module-symbol inference. Unknown flavor runs only the
// flavor-independent shapes (CAS loops, refcount atomics, guarded delete).
//
// Guarantees: only airtight matches rewrite (every memory access in the
// consumed region is accounted for by the matched idiom), `!revenant.va`
// provenance is copied onto every emitted call, `!revenant.opaque` and
// `!revenant.region` are never dropped, and output stays LLVM 22
// parseable. Non-matching code is left untouched.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_TRANSFORMS_UNCXX_UNCXX_H
#define REVENANT_TRANSFORMS_UNCXX_UNCXX_H

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class Module;
}

namespace revenant {

/// STL implementation flavor. `Auto` infers from module symbols and target.
enum class CxxFlavor { Auto, Msvc, Gnu, Libcxx };

class UncxxPass : public llvm::PassInfoMixin<UncxxPass> {
  CxxFlavor Flavor = CxxFlavor::Auto;
  // The user functions the pass may rewrite, by name. The names survive the
  // stage copies (a stage that fails is restored by reparsing its input
  // text, which builds a new module with new function objects), so a set of
  // function pointers would silently stop matching -- and match by accident
  // when the allocator reuses an address.
  const llvm::StringSet<> *UserNames = nullptr;
  // The STL flavor is a property of the module's symbols, but inferring it
  // scans every function and global name. The pass object outlives the
  // per-function runs, so the inference is done once per module.
  CxxFlavor CachedFlavor = CxxFlavor::Auto;
  const llvm::Module *CachedModule = nullptr;
  bool FlavorResolved = false;

public:
  explicit UncxxPass(CxxFlavor F = CxxFlavor::Auto,
                     const llvm::StringSet<> *Users = nullptr)
      : Flavor(F), UserNames(Users) {}
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return false; }
};

} // namespace revenant

#endif // REVENANT_TRANSFORMS_UNCXX_UNCXX_H
