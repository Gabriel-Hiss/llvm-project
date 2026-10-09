//==- revenant/lib/Analysis/Xrefs.cpp - Xref table builder ---------------==//
//
// See revenant/Analysis/Xrefs.h.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Xrefs.h"

#include "revenant/Disasm/Disasm.h"

#include <map>
#include <utility>
#include <vector>

namespace revenant {
namespace {

// RVA of `Addr`, saturating to UINT32_MAX outside the u32 range below the
// base (Rust: `u32::try_from(va.0 - image.base.0).unwrap_or(u32::MAX)`).
Rva rvaOf(Va Base, Va Addr) {
  auto Off = Addr.offsetFrom(Base);
  if (!Off || *Off > UINT32_MAX)
    return UINT32_MAX;
  return (Rva)*Off;
}

// Reference class for one operand target: direct kinds map 1:1, plain data
// splits into Read/Write. Rust consults the instruction's memory-access
// flags here; the MC counterpart is whether the opcode may store.
XrefKind classifyRef(const Disasm &D, const Instr &I, RefKind Kind) {
  switch (Kind) {
  case RefKind::Call:
    return XrefKind::Call;
  case RefKind::Jump:
    return XrefKind::Jump;
  case RefKind::AddrOf:
    return XrefKind::AddrOf;
  case RefKind::Data:
    return D.isStore(I) ? XrefKind::Write : XrefKind::Read;
  }
  return XrefKind::Read;
}

// Record one reference, dropping targets that map to no section (Rust
// `push_xref`'s `section_of` gate).
void pushXref(std::map<Va, std::vector<Xref>> &Xrefs, const Image &Img,
              Va Target, XrefKind Kind, Va From) {
  if (!Img.sectionOf(Target))
    return;
  Xrefs[Target].push_back({From, Kind});
}

// Flatten in target order into the compact pools (Rust's BTreeMap iteration
// is already sorted, like std::map here).
Xrefs flatten(const Image &Img, std::map<Va, std::vector<Xref>> &ByTarget) {
  Xrefs Out;
  Out.Base = Img.Base;
  Out.Offsets.push_back(0);
  for (auto &[Target, Refs] : ByTarget) {
    Out.Targets.push_back(rvaOf(Img.Base, Target));
    for (const Xref &R : Refs) {
      Out.Froms.push_back(rvaOf(Img.Base, R.From));
      Out.Kinds.push_back(R.Kind);
    }
    Out.Offsets.push_back((uint32_t)Out.Froms.size());
  }
  return Out;
}

} // end anonymous namespace

Xrefs collectXrefs(const Image &Img, llvm::ArrayRef<Function> Funcs,
                   const Disasm &D) {
  std::map<Va, std::vector<Xref>> ByTarget;
  for (const Function &F : Funcs)
    for (const Instr &I : F.Instrs)
      for (auto [Target, Kind] : D.refs(I))
        pushXref(ByTarget, Img, Target, classifyRef(D, I, Kind), I.Addr);
  return flatten(Img, ByTarget);
}

Xrefs collectXrefs(const Image &Img, llvm::ArrayRef<Instr> Instrs,
                   const Disasm &D) {
  std::map<Va, std::vector<Xref>> ByTarget;
  for (const Instr &I : Instrs)
    for (auto [Target, Kind] : D.refs(I))
      pushXref(ByTarget, Img, Target, classifyRef(D, I, Kind), I.Addr);
  return flatten(Img, ByTarget);
}

} // end namespace revenant
