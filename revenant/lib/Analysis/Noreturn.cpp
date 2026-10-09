//===- revenant/lib/Analysis/Noreturn.cpp - Noreturn detection -----------===//
//
// Port of crates/analysis/src/noreturn.rs: known imports, panic primitives,
// and a fixpoint over the call graph. Trap classification (ud0/ud1/ud2,
// int3, int 0x29) comes from Discovery's `TargetHooks`; the call-target
// helper is Discovery's `callSiteTarget` (`revenant/Analysis/Analysis.h`),
// injected here so this slice keeps no duplicate of it.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Noreturn.h"
#include "revenant/Target/TargetHooks.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace revenant {

namespace {

/// Noreturn API names (compared against the `dll!name` short form).
constexpr const char *NoreturnApis[] = {
    "ExitProcess",
    "TerminateProcess",
    "abort",
    "exit",
    "_exit",
    "_Exit",
    "__fastfail",
    "RaiseException",
    "__report_gsfailure",
    "__int2c",
    "panic_abort",
    "KeBugCheckEx",
    "RtlFailFast2",
    "FreeLibraryAndExitThread",
    "_invoke_watson",
};

/// Noreturn by demangled-name substring (Rust panic paths etc.).
constexpr const char *NoreturnNameBits[] = {
    "core::panicking",
    "std::process::abort",
    "rust_panic",
    "__rust_start_panic",
    "rust_begin_unwind",
};

/// Short-form import match: exact `name`, decorated `name@...`, or
/// qualified `...::name`.
bool shortMatches(llvm::StringRef Sym, llvm::StringRef Api) {
  if (Sym == Api)
    return true;
  if (Sym.size() > Api.size() && Sym.starts_with(Api) &&
      Sym[Api.size()] == '@')
    return true;
  if (Sym.size() > Api.size() + 2 && Sym.ends_with(Api) &&
      Sym.drop_back(Api.size()).ends_with("::"))
    return true;
  return false;
}

/// A way function code stops executing: a tail exit to another VA, an exit
/// already proven dead, or an exit whose fate can't be proven.
struct Exit {
  enum class Tag : uint8_t { Call, Dead, Unknown } Kind = Tag::Unknown;
  /// Target for `Tag::Call` (tail call / cross-function branch).
  Va Target;
};

/// Discovery's call-target helper, injected (declared in
/// `revenant/Analysis/Analysis.h`, owned by Swe2Discovery).
using CallSiteFn = std::optional<Va> (*)(const Instr &);

/// Terminal trap per `TargetHooks::isNoreturnTrap` (ud0/ud1/ud2, int3,
/// int 0x29), or anything the MC layer already files as a barrier trap.
/// The block ends dead on its own, no callee verdict.
bool isTrap(const Instr &I, const TargetHooks &Hooks) {
  if (I.Flow == FlowKind::Trap)
    return true;
  return Hooks.isNoreturnTrap(I);
}

/// Every exit out of `F`. Internal flow (fallthrough, intra-function jumps
/// and calls that continue) is not an exit. A `Noreturn` block is a proven
/// non-returning exit only when re-proven here: the callee may no longer be
/// known noreturn, so a plain `call` tail re-resolves to its target and
/// anything else unprovable (empty block, non-trap tail) is Unknown.
std::vector<Exit> exitsOf(const Function &F, const TargetHooks &Hooks,
                          CallSiteFn CallSiteTarget) {
  std::vector<Exit> Out;
  for (const BasicBlock &B : F.Blocks) {
    switch (B.EndKind) {
    case BlockEnd::UnresolvedIndirect:
    case BlockEnd::EndOfFunction:
      Out.push_back({Exit::Tag::Unknown, Va()});
      break;
    // `jmp` out of the region: the block's TailCall edge is the exit. A
    // conditional branch may also carry a TailCall edge when the taken side
    // leaves the region.
    case BlockEnd::TailCall:
    case BlockEnd::Branch:
      for (const Edge &E : B.Edges)
        if (E.Kind == EdgeKind::TailCall)
          Out.push_back({Exit::Tag::Call, E.To});
      break;
    case BlockEnd::Noreturn: {
      if (B.InstrsEnd == B.InstrsBegin || B.InstrsEnd > F.Instrs.size()) {
        Out.push_back({Exit::Tag::Unknown, Va()});
        continue;
      }
      const Instr &Last = F.Instrs[B.InstrsEnd - 1];
      if (isTrap(Last, Hooks)) {
        Out.push_back({Exit::Tag::Dead, Va()});
        continue;
      }
      if (auto T = CallSiteTarget(Last))
        Out.push_back({Exit::Tag::Call, *T});
      else
        Out.push_back({Exit::Tag::Unknown, Va()});
      break;
    }
    case BlockEnd::Fallthrough:
    case BlockEnd::Return:
      break;
    }
  }
  return Out;
}

/// Fallback call-target matching Discovery's contract (direct call target,
/// else RIP-relative slot) for builds and tests without Analysis.h.
std::optional<Va> defaultCallSiteTarget(const Instr &I) {
  if (I.Flow != FlowKind::Call)
    return std::nullopt;
  if (I.HasTarget)
    return I.Target;
  if (I.HasMemTarget)
    return I.MemTarget;
  return std::nullopt;
}

std::set<Va> propagateImpl(std::vector<Function> &Funcs,
                           const std::map<Va, Extern> &Externs,
                           const std::map<Va, std::string> &Names,
                           const TargetHooks &Hooks, CallSiteFn CallSite) {
  // Reset inferred flags from any earlier pass: a callee that turned
  // returning since then must stop poisoning its callers. Only explicit
  // name proofs reapply.
  std::set<Va> Noreturn;
  for (Function &F : Funcs) {
    auto It = Names.find(F.Start);
    F.IsNoreturn = It != Names.end() && nameIsNoreturn(It->second);
    if (F.IsNoreturn)
      Noreturn.insert(F.Start);
  }

  auto staticNr = [&](Va T) {
    auto E = Externs.find(T);
    if (E != Externs.end() && E->second.Noreturn)
      return true;
    auto N = Names.find(T);
    if (N != Names.end() && nameIsNoreturn(N->second))
      return true;
    return Noreturn.count(T) != 0;
  };

  // Worklist: a candidate (no `ret`, no unknown exit, >=1 exit) turns
  // noreturn once every tail target is proven noreturn. `Pending[c]`
  // counts unproven tail targets; `Waiters[t]` lists candidates waiting
  // on `t`.
  std::vector<size_t> Pending(Funcs.size(), 0);
  std::map<Va, std::vector<size_t>> Waiters;
  std::vector<size_t> Ready;
  for (size_t Ci = 0, N = Funcs.size(); Ci < N; ++Ci) {
    const Function &F = Funcs[Ci];
    if (F.IsNoreturn)
      continue;
    bool HasRet = false;
    for (const Instr &I : F.Instrs)
      if (I.Flow == FlowKind::Return) {
        HasRet = true;
        break;
      }
    if (HasRet)
      continue;
    std::vector<Exit> Exits = exitsOf(F, Hooks, CallSite);
    // No exits: infinite loop or halt. Conservative: not noreturn.
    if (Exits.empty())
      continue;
    bool HasUnknown = false;
    for (const Exit &E : Exits)
      if (E.Kind == Exit::Tag::Unknown) {
        HasUnknown = true;
        break;
      }
    if (HasUnknown)
      continue;
    size_t NrCalls = 0;
    for (const Exit &E : Exits) {
      if (E.Kind == Exit::Tag::Dead)
        ++NrCalls;
      else if (E.Kind == Exit::Tag::Call && staticNr(E.Target))
        ++NrCalls;
      else if (E.Kind == Exit::Tag::Call) {
        ++Pending[Ci];
        Waiters[E.Target].push_back(Ci);
      }
    }
    if (Pending[Ci] == 0 && NrCalls > 0)
      Ready.push_back(Ci);
  }
  while (!Ready.empty()) {
    size_t Ci = Ready.back();
    Ready.pop_back();
    Function &F = Funcs[Ci];
    F.IsNoreturn = true;
    Noreturn.insert(F.Start);
    auto W = Waiters.find(F.Start);
    if (W == Waiters.end())
      continue;
    for (size_t Dw : W->second) {
      if (Pending[Dw] == 0)
        continue;
      if (--Pending[Dw] == 0)
        Ready.push_back(Dw);
    }
    Waiters.erase(W);
  }
  return Noreturn;
}

} // end anonymous namespace

bool isNoreturnImport(llvm::StringRef Name) {
  size_t Bang = Name.rfind('!');
  llvm::StringRef Sym =
      (Bang == llvm::StringRef::npos) ? Name : Name.drop_front(Bang + 1);
  for (const char *Api : NoreturnApis)
    if (shortMatches(Sym, Api))
      return true;
  for (const char *Bit : NoreturnNameBits)
    if (Name.contains(Bit))
      return true;
  return false;
}

bool nameIsNoreturn(llvm::StringRef Name) {
  for (const char *Bit : NoreturnNameBits)
    if (Name.contains(Bit))
      return true;
  return false;
}

std::set<Va> propagateNoreturn(std::vector<Function> &Funcs,
                               const std::map<Va, Extern> &Externs,
                               const std::map<Va, std::string> &Names,
                               const TargetHooks &Hooks) {
  return propagateImpl(Funcs, Externs, Names, Hooks, defaultCallSiteTarget);
}

} // end namespace revenant
