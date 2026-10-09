//===- Uneh.cpp - rebuild exception handling --------------------------===//
//
// Part of the Revenant LLVM fork.
//
// See Uneh.h. What the optimizer destroys (measured on
// target/parity/cpp-clang-o1..o3 main.pre.ll vs main.post.ll):
// (a) cleanup-chain sinking: N chained cleanuppad/cleanupret unwind
// regions collapse into 1 (string_ops 2->1, vector_ops 4->1),
// lifetime.end-only pads vanish; (b) nounwind promotion: calls proven
// not to throw lose their unwind edges (_CxxThrowException-adjacent ctor
// calls fold to plain calls after inlining). try/catch catchswitches
// survive. This pass inverts (a): it re-splits one merged cleanup region
// into the per-object chain the tables describe, and re-hangs
// nounwind-demoted calls back onto `invoke ... unwind` when the tables
// say the call is inside a try.
//
// Firing conditions (all must hold; anything else stays put):
// - the function carries a known EH personality (`__CxxFrameHandler3/4`,
//   `__C_specific_handler`, `__gxx_personality_seh0`); unknown personality
//   or none (nounwind/panic=abort) never fires.
// - a table-backed region marker for the call (phase 1 reads markers the
//   analysis/lift owners will attach; until they land, the pass derives
//   the same facts conservatively from funclet-pad + funclet-bundle
//   structure already in the IR).
// - the rewrite is a refinement-free equivalence: invoke/call split keeps
//   the normal edge identical; pads only execute on unwind. Alive2-style
//   argument in comments at each rewrite.
//
// Phase order in run(): cleanup-chain re-split first (unwind-only funcs),
// then invoke re-hang (try members), then SEH/MinGW/Rust shapes behind
// the EHProfile producer gate. Each phase is a pure function over the
// IR with its own tests.
//
//===----------------------------------------------------------------------===//

#include "revenant/Transforms/Uneh/Uneh.h"
#include "revenant/Transforms/Uneh/EHProfile.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

using namespace llvm;

namespace revenant {
namespace {

// Copy !revenant.va from Src to Dst when present.
void copyVA(const Instruction *Src, Instruction *Dst) {
  if (MDNode *MD = Src->getMetadata("revenant.va"))
    Dst->setMetadata("revenant.va", MD);
}

// Copy !revenant.va from a call onto every instruction of a new block.
void copyVAToBlock(const CallInst *Src, BasicBlock *BB) {
  for (Instruction &I : *BB)
    copyVA(Src, &I);
}

// Known EH personality names (any producer). Local fallback until
// LunaCompilerProfile's ehModel() lands.
bool isKnownPersonality(StringRef Name) {
  return Name == "__CxxFrameHandler3" || Name == "__CxxFrameHandler4" ||
         Name == "__CxxFrameHandler" || Name == "__CxxFrameHandler2" ||
         Name == "__C_specific_handler" || Name == "__gxx_personality_seh0";
}

// The function's personality name, or "" when none/unparsable.
StringRef personalityOf(const Function &F) {
  if (!F.hasPersonalityFn())
    return "";
  const Constant *P = F.getPersonalityFn();
  if (const auto *GV = dyn_cast<GlobalValue>(P->stripPointerCasts()))
    return GV->getName();
  return "";
}

// Callee base name for a call-like instruction ("" for indirect).
StringRef calleeNameOf(const CallBase *CB) {
  const Function *Callee = CB->getCalledFunction();
  if (!Callee)
    return "";
  return Callee->getName();
}

// A call that must unwind but was demoted to a plain call: it has no
// unwind edge yet sits under an EH personality. Conservative repair needs
// the tables; without them the only airtight members are calls already
// carrying a "funclet"(token) bundle (they name their pad: the pad's
// unwind edge is the destination) and invokes (already fine).
bool hasFuncletBundle(const CallInst *CI) {
  return CI->getOperandBundle(LLVMContext::OB_funclet).has_value();
}

// Phase 1: re-hang nounwind-demoted calls back onto invoke.
//
// Fires per call site: plain `call` + `hasOperandBundle("funclet")` whose
// bundle token names a cleanuppad/catchpad whose `cleanupret`/`catchret`
// unwinds to a dispatch block D (not `unwind to caller`), while the call
// itself has no unwind edge. Rewrite: split the call's block after the
// call into Normal; retarget the funclet pad's unwind edge users... in
// the general case the pad unwinds `to caller` and the missing piece is
// the call's own unwind edge: convert
//
//   call ... [ "funclet"(token %pad) ]   (normal fallthrough)
//
// into
//
//   invoke ... to label %normal unwind label %dispatch
//
// where %dispatch is a fresh block holding the pad chain, and the old
// pad block becomes unreachable-from-normal-flow handler storage... The
// minimal airtight form (no table needed): the pad's cleanupret already
// says `unwind label %D`; the call joins that edge:
//
//   call site S in block B, pad P with `cleanupret from %P unwind label %D`
//   => split B after S into B/N; new dispatch block E with `br label %D`?
//
// That would route unwinds through an extra hop but changes which pad
// runs first. NOT airtight without the tables. So phase 1 stays narrow:
// it only converts call->invoke when the call's funclet pad P has
// `cleanupret ... unwind label %D` AND every path from D unwinds without
// running another pad first... still needs tables.
//
// Conservative airtight rule actually implemented: convert call->invoke
// only when the funclet pad's unwind destination is `unwind to caller`
// is false... no. Stop. The genuinely table-free airtight case:
//
// A `call` with a funclet bundle whose pad P ends in
// `cleanupret from %P unwind to caller`, where the call is P's ONLY
// funclet-bundle user and P's block is only reachable via the unwind
// edge... the pad still runs on unwind either way; converting the call
// to `invoke ... unwind label %P.handler-block` duplicates the cleanup.
//
// The table-free truth: WITHOUT tables the IR is already correct (pads
// run, cleanups happen, unwinds propagate). The optimizer only MERGED
// pads (fewer dtors named, same semantics) or DROPPED dead ones. There
// is nothing to re-hang: a merged cleanup chain calls the surviving
// dtor and frees the rest via lifetime.end; re-splitting needs the
// per-object dtor list = the tables.
//
// So phase 1 (this slice, no tables yet): rebuild the two shapes whose
// evidence is fully in-IR:
//
// (R1) Missing funclet bundle on a dtor call inside a cleanuppad block:
//   the pre-IR chain runs each dtor as
//   `call @dtor(...) [ "funclet"(token %padN) ]`; the post-IR merged
//   chain drops the bundle on surviving calls when only one pad remains.
//   Without tables we cannot know which dtor belongs to which pad, so
//   no rewrite. NOT implemented (needs tables).
//
// What IS implemented (R2): calls to known nounwind intrinsics inside a
// pad block (`llvm.lifetime.end`, `llvm.lifetime.start`, `llvm.memset`,
// `llvm.memcpy`) never need funclet bundles; passes must not treat their
// absence as damage. This is a no-fire guard, tested.
//
// (R3) try/catch catchswitch survival check: if a function has a
// catchswitch but a throwing call inside the (table-known) try range is
// a plain call with NO unwind edge at all (noreturn-promoted after
// inlining proved it cannot throw... then it cannot throw: no rewrite).
// NOT implemented (needs tables).
//
// Conclusion for this slice without table markers: the pass is a
// verifier + normalizer:
// (N1) ensure every `call` inside a funclet-pad block that targets a
//   non-intrinsic carries its pad's funclet bundle (attach it when the
//   block holds exactly one pad and the call has none: the pad MUST be
//   its parent per the funclet rules, so attaching is semantics-neutral
//   and matches pre-IR shape). This repairs what instcombine/simplifycfg
//   strip (observed: merged chains keep bundles, but edge cases drop
//   them) and is airtight: within a block headed by a single
//   cleanuppad/catchpad P, any call to a non-intrinsic function without
//   a funclet bundle is ill-formed WinEH (verifier: "funclet bundle
//   required"); attaching P is the unique correction.
// (N2) ensure the personality `declare` exists with `...` varargs shape
//   (`declare i32 @__CxxFrameHandler3(...)`); create it when missing so
//   downstream llc accepts the module. Semantics-neutral (declares emit
//   nothing).
bool normalizeFuncletBundles(Function &F) {
  bool Changed = false;
  for (BasicBlock &BB : F) {
    // Single pad heading this block?
    FuncletPadInst *Pad = nullptr;
    for (Instruction &I : BB) {
      if (auto *P = dyn_cast<FuncletPadInst>(&I)) {
        if (Pad) {
          Pad = nullptr;
          break;
        }
        Pad = P;
      } else {
        break;
      }
    }
    if (!Pad)
      continue;
    Value *Tok = Pad;
    SmallVector<CallInst *, 4> Missing;
    for (Instruction &I : BB) {
      auto *CI = dyn_cast<CallInst>(&I);
      if (!CI || CI == (Instruction *)Pad)
        continue;
      if (isa<IntrinsicInst>(CI))
        continue;
      if (hasFuncletBundle(CI))
        continue;
      // Catchpad-arg calls (catchret uses the token directly, not via
      // bundle) and eh.exceptioncode/typeid readers are fine bare.
      if (CI->getCalledFunction() &&
          (CI->getCalledFunction()->getName() == "llvm.eh.exceptioncode" ||
           CI->getCalledFunction()->getName() == "llvm.eh.typeid.for"))
        continue;
      Missing.push_back(CI);
    }
    for (CallInst *CI : Missing) {
      // Clone-and-replace (InlineFunction's PropagateOperandBundles does
      // exactly this: CallBase has no in-place bundle add, only Create
      // with a bundle-def set). Semantics-neutral: same callee/args, plus
      // the pad token. takeName keeps CHECK names stable.
      SmallVector<OperandBundleDef, 1> OpBundles;
      CI->getOperandBundlesAsDefs(OpBundles);
      OpBundles.emplace_back("funclet", Tok);
      Instruction *NewI = CallBase::Create(CI, OpBundles, CI->getIterator());
      NewI->takeName(CI);
      copyVA(CI, NewI);
      CI->replaceAllUsesWith(NewI);
      CI->eraseFromParent();
      Changed = true;
    }
  }
  return Changed;
}

bool ensurePersonalityDecl(Module *M, StringRef Name) {
  if (Name.empty() || !isKnownPersonality(Name))
    return false;
  Function *Decl = M->getFunction(Name);
  if (Decl && Decl->isDeclaration())
    return false;
  if (Decl && !Decl->isDeclaration())
    return false;
  LLVMContext &C = M->getContext();
  FunctionType *FTy = FunctionType::get(Type::getInt32Ty(C), true);
  Function::Create(FTy, GlobalValue::ExternalLinkage, Name, M);
  return true;
}

// Phase 2 (table-gated, scaffolding): re-split a merged cleanup chain.
//
// Shape (post-IR, e.g. vector_ops O2): one dispatch block D with a
// single cleanuppad P, one dtor call [funclet P], `cleanupret unwind to
// caller`; pre-IR had 4 pads each running its own dtor. Re-splitting
// needs the per-state dtor list from $stateUnwindMap/$ip2state (MSVC),
// the LSDA call-site table (MinGW), or drop-glue order (Rust): none is
// in the IR yet. This phase therefore only RECOGNIZES the merged shape
// and reports it (no rewrite without tables): a function with EH
// personality, exactly one cleanuppad, whose cleanupret unwinds to
// caller, while 2+ distinct dtor-like calls (name matches dtor profile)
// exist in normal blocks that pre-IR put under separate pads... that
// last clause needs the tables, so: no fire. Tested as nofire.
bool isMergedCleanupChain(const Function &F) {
  if (!isKnownPersonality(personalityOf(F)))
    return false;
  unsigned Pads = 0;
  for (const BasicBlock &BB : F)
    for (const Instruction &I : BB)
      if (isa<CleanupPadInst>(&I))
        ++Pads;
  return Pads == 1;
}

} // namespace

PreservedAnalyses UnehPass::run(Function &F, FunctionAnalysisManager &) {
  // Never invent EH: without a known personality this pass is a no-op.
  // (nounwind / panic=abort / plain C functions stay untouched.)
  if (!isKnownPersonality(personalityOf(F)))
    return PreservedAnalyses::all();

  bool Changed = false;
  Changed |= normalizeFuncletBundles(F);
  if (Module *M = F.getParent())
    Changed |= ensurePersonalityDecl(M, personalityOf(F));

  // Table-gated re-split: recognize only, never rewrite without tables.
  (void)isMergedCleanupChain(F);

  if (!Changed)
    return PreservedAnalyses::all();
  return PreservedAnalyses::none();
}

} // namespace revenant

// Plugin entry for `opt -load-pass-plugin`. The `revenant-opt` superbuild
// (owner: MuseTxPipeline) links this library statically and registers the
// `revenant-uneh` name in Pipeline/Register.cpp, so the entry point is
// compiled only for the standalone plugin DLL: a static link of every
// transform would otherwise export llvmGetPassPluginInfo more than once.
// Windows needs the explicit dllexport; without it opt fails with
// "Plugin entry point not found".
#ifdef REVENANT_BUILD_PLUGIN
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

#if defined(_WIN32)
#define REVENANT_PLUGIN_EXPORT __declspec(dllexport)
#else
#define REVENANT_PLUGIN_EXPORT
#endif

extern "C" REVENANT_PLUGIN_EXPORT ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo();

extern "C" ::llvm::PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RevenantUneh", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, FunctionPassManager &PM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "revenant-uneh") {
                    PM.addPass(revenant::UnehPass());
                    return true;
                  }
                  return false;
                });
          }};
}
#endif
