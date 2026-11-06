//===- PairReturns.h - rax:rdx return evidence from call sites -*- C++ -*-===//
//
// rustc returns scalar pairs (slices, `Option<usize>`, `(T, U)` of two
// words) in rax:rdx. Win64 makes rdx volatile, so a caller that reads rdx
// after a call, before writing it, reads the callee's second result word.
// The scanner reports the direct call targets whose callers do that; the
// driver lifts those callees with a `{i64, i64}` return.
//
//===----------------------------------------------------------------------===//
#ifndef REVENANT_LIB_CAPI_PAIRRETURNS_H
#define REVENANT_LIB_CAPI_PAIRRETURNS_H

#include "revenant/Core/Program.h"

#include "llvm/ADT/DenseSet.h"

#include <memory>

namespace llvm {
class MCInstrInfo;
class MCRegisterInfo;
} // namespace llvm

namespace revenant {

class PairReturnScanner {
public:
  PairReturnScanner();
  ~PairReturnScanner();
  /// Adds to `Targets` every direct call target in `F` whose result rdx
  /// the code after the call reads.
  void scan(const Function &F, llvm::DenseSet<uint64_t> &Targets) const;

private:
  std::unique_ptr<llvm::MCInstrInfo> MII;
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
};

} // namespace revenant

#endif
