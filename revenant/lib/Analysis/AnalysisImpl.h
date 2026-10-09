// Internal helpers shared by analysis translation units.
#ifndef REVENANT_LIB_ANALYSIS_ANALYSISIMPL_H
#define REVENANT_LIB_ANALYSIS_ANALYSISIMPL_H

#include "revenant/Analysis/Analysis.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Target/TargetHooks.h"

#include <map>
#include <set>
#include "llvm/ADT/STLFunctionalExtras.h"

namespace revenant::analysis_detail {

struct OverlapEnv {
  llvm::ArrayRef<Va> WeakIps;
  bool Bounded = false;
  std::set<Va> *DeadStarts = nullptr;
  llvm::function_ref<bool(Va)> ForeignVeto;
};

struct BuildResult {
  Function F;
  std::vector<Va> LeaTargets;
  std::vector<Va> RipRefs;
  std::vector<Va> ForeignTargets;
};

BuildResult buildFunction(
    const Image &Img, const Disasm &Dis, const TargetHooks &Hooks, Va Start,
    const std::set<Va> &FunctionStarts,
    const std::map<Va, std::pair<Va, Va>> &Pdata,
    const std::map<Va, std::vector<std::pair<Va, Va>>> &PdataGroups,
    const std::set<Va> &SharedRipRefs,
    llvm::function_ref<bool(Va)> IsNoreturn, bool DiscoveryOnly = false,
    const std::map<Va, Va> *ClaimedCode = nullptr,
    llvm::ArrayRef<std::pair<Va, Va>> Bound = {});


std::set<Va> collectStrongStarts(const PeFile &Pe, const Image &Img);
std::set<Va> collectInitialWeakCandidates(const PeFile &Pe);
bool rejectWeakCandidate(const Image &Img, const Disasm &Dis,
                         const TargetHooks &Hooks,
                         const std::map<Va, std::pair<Va, Va>> &Pdata,
                         llvm::ArrayRef<std::pair<Va, Va>> Claimed, Va Candidate);
bool rejectWeakFunction(const Image &Img, const TargetHooks &Hooks,
                        const Function &F);

void resolveOverlaps(Function &F, const Image &Img, const Disasm &Dis,
                     const TargetHooks &Hooks);
void resolveOverlaps(Function &F, const Image &Img, const Disasm &Dis,
                     const TargetHooks &Hooks, const OverlapEnv &Env);
void pruneDataInstrs(Function &F, const Image &Img);
void buildChunks(Function &F, llvm::ArrayRef<std::pair<Va, Va>> OwnRanges);

} // namespace revenant::analysis_detail

#endif
