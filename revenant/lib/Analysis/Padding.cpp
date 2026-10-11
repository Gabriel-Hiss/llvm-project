//==- lib/Analysis/Padding.cpp - Alignment filler recovery -------------==//
//
// Port of the padding classifier in crates/analysis/src/lib.rs
// (`is_filler`, `pad_len`, `filler_head`, `gap_fillers`,
// `decode_padding`). One deliberate addition over the reference: `isFiller`
// also accepts the 5-byte `call $+5` (`E8 00 00 00 00`) the toolchain
// leaves as alignment filler.
//
// Opcode identity comes from `MCInstrInfo` names (`NOOP*`, `INT3`,
// `XCHG*`), so no generated target headers are needed; register identity
// for `xchg ax,ax` comes from `MCRegisterInfo` names.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/Padding.h"

#include "revenant/Core/Image.h"
#include "revenant/Core/Instr.h"
#include "revenant/Disasm/Disasm.h"
#include "revenant/Object/PeFile.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/TargetParser/Triple.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <utility>

namespace revenant {

namespace {

// X86 MC tables for opcode/register names. Built once; `Disasm` owns its
// own copies for decoding, these are only used to classify.
struct X86Tables {
  std::unique_ptr<llvm::MCRegisterInfo> MRI;
  std::unique_ptr<llvm::MCInstrInfo> MII;
};

const X86Tables &x86Tables() {
  static std::once_flag Flag;
  static std::unique_ptr<X86Tables> Tables;
  std::call_once(Flag, [] {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    llvm::Triple TT("x86_64-pc-windows-msvc");
    std::string Err;
    const llvm::Target *T = llvm::TargetRegistry::lookupTarget(TT, Err);
    auto X = std::make_unique<X86Tables>();
    if (T) {
      X->MRI.reset(T->createMCRegInfo(TT));
      X->MII.reset(T->createMCInstrInfo());
    }
    Tables = std::move(X);
  });
  return *Tables;
}

llvm::StringRef opcodeName(const Instr &I) {
  const X86Tables &T = x86Tables();
  if (!T.MII)
    return {};
  return T.MII->getName(I.Inst.getOpcode());
}

std::string upperRegName(llvm::MCRegister R) {
  const X86Tables &T = x86Tables();
  std::string S = T.MRI ? std::string(T.MRI->getName(R)) : std::string();
  for (char &C : S)
    C = (char)std::toupper((unsigned char)C);
  return S;
}

// `xchg r,r` with both halves the same ax-family register: semantically a
// nop. The reference only names `ax,ax` (`66 90`); accepting `eax`/`rax`
// too covers however MC spells the single-byte `90` form.
bool isAxXchg(const Instr &I) {
  // `xchg eax,[mem]` carries the tied register as a second operand; only the
  // register-register spellings are fillers.
  llvm::StringRef Name = opcodeName(I);
  if (Name.ends_with("rm") || Name.ends_with("mr"))
    return false;
  if (I.Inst.getNumOperands() < 2 || !I.Inst.getOperand(0).isReg() ||
      !I.Inst.getOperand(1).isReg())
    return false;
  std::string A = upperRegName(I.Inst.getOperand(0).getReg());
  std::string B = upperRegName(I.Inst.getOperand(1).getReg());
  return A == B && (A == "AX" || A == "EAX" || A == "RAX");
}

Rva rvaOrMax(const Image &Img, Va V) { return Img.rvaOf(V).value_or(UINT32_MAX); }

bool isAlignedEnd(uint64_t VA) { return (VA & 15) == 0; }

} // end anonymous namespace

bool isFiller(const Instr &I, const Image &Img) {
  (void)Img;
  llvm::StringRef Name = opcodeName(I);
  // Any nop form, including multi-byte `0F 1F` and `66 90`-family spellings
  // MC reports under a NOOP opcode.
  if (Name.starts_with("NOOP"))
    return true;
  if (Name == "INT3")
    return true;
  if (Name.starts_with("XCHG") && isAxXchg(I))
    return true;
  return false;
}

bool isFiller(const Instr &I) {
  llvm::StringRef Name = opcodeName(I);
  if (Name.starts_with("NOOP"))
    return true;
  if (Name == "INT3")
    return true;
  if (Name.starts_with("XCHG") && isAxXchg(I))
    return true;
  return false;
}

bool isFillerExt(const Instr &I) {
  if (isFiller(I))
    return true;
  // `call $+5` (`E8 00 00 00 00`): a direct 5-byte call whose target is
  // the next instruction. Only the `E8 disp32=0` encoding satisfies all
  // three at once. Opt-in only; the decode path never calls this.
  if (I.Flow == FlowKind::Call && I.HasTarget && I.Size == 5 &&
      I.Target == I.nextVa())
    return true;
  return false;
}

uint64_t padLen(const Disasm &Dis, const Image &Img, Va Addr) {
  auto B = Img.read(Addr, 1);
  if (!B || B->empty())
    return 0;
  if ((*B)[0] == 0x00 || (*B)[0] == 0xCC)
    return 1;
  auto I = Dis.decodeOne(Addr);
  if (I && isFiller(*I, Img))
    return I->Size;
  return 0;
}

std::optional<Va> fillerHead(const Disasm &Dis, Va Addr) {
  const Image &Img = Dis.image();
  std::optional<Va> Head;
  for (uint64_t K = 1; K < 15; ++K) {
    if (Addr.Value < K)
      break;
    Va X(Addr.Value - K);
    auto I = Dis.decodeOne(X);
    if (I && isFiller(*I, Img) && I->nextVa() > Addr)
      Head = X;
  }
  if (!Head)
    return std::nullopt;
  Va H = *Head;
  for (;;) {
    std::optional<Va> Prev;
    for (uint64_t K = 1; K < 15; ++K) {
      if (H.Value < K)
        break;
      Va X(H.Value - K);
      auto I = Dis.decodeOne(X);
      if (I && isFiller(*I, Img) && I->nextVa() == H)
        Prev = X;
    }
    if (!Prev)
      break;
    H = *Prev;
  }
  return H;
}

bool startsAfterInt3(const Disasm &Dis, const Image &Img, Va Addr) {
  // The 16-byte boundary plus an int3 run is the MSVC x64 inter-function
  // layout: every function start is aligned and the alignment gap between
  // two functions is int3, while loops and cold blocks *inside* a function
  // are aligned with nop padding. The int3 run must also sit directly
  // after a `ret`: a function that ends in `ret` is closed, so the next
  // aligned address is a new function. An unconditional `jmp` over int3
  // padding is the same shape inside one function (a jump to an aligned
  // continuation block), and must not split the function.
  if (Addr.Value & 15)
    return false;
  Va Cur = Addr;
  bool Found = false;
  for (unsigned Guard = 0; Guard < 64 && Cur.Value > 0; ++Guard) {
    auto B = Img.read(Va(Cur.Value - 1), 1);
    if (!B || B->empty() || (*B)[0] != 0xCC)
      break;
    Cur = Va(Cur.Value - 1);
    Found = true;
  }
  if (!Found || Cur.Value == 0)
    return false;
  auto Before = Img.read(Va(Cur.Value - 1), 1);
  (void)Dis;
  return Before && !Before->empty() && (*Before)[0] == 0xC3;
}

void gapFillers(const Disasm &Dis, const Image &Img, Va Start, Va End,
                std::vector<PaddingRun> &Out) {
  std::vector<Rva> Instrs;
  Va Ip = Start;
  while (Ip < End) {
    auto I = Dis.decodeOne(Ip);
    if (!I)
      break;
    if (!isFiller(*I, Img) || I->nextVa() > End)
      break;
    Instrs.push_back(rvaOrMax(Img, Ip));
    Ip = I->nextVa();
  }
  if (Ip == End) {
    // Whole piece is filler: the classic alignment gap.
    Out.push_back({rvaOrMax(Img, Start), rvaOrMax(Img, End),
                   std::move(Instrs)});
    return;
  }
  // Otherwise the piece holds something else (data, a table) after the
  // filler. Keep only a leading run that starts exactly at the chunk end
  // and closes on an alignment boundary. No interior or suffix runs.
  if (!Instrs.empty() && isAlignedEnd(Ip.Value))
    Out.push_back({rvaOrMax(Img, Start), rvaOrMax(Img, Ip),
                   std::move(Instrs)});
}

std::vector<PaddingRun>
decodePadding(const PeFile &Pe,
              llvm::ArrayRef<std::pair<Va, Va>> Chunks,
              llvm::ArrayRef<std::pair<Va, Va>> Data) {
  const Image &Img = *Pe.Img;
  llvm::Expected<Disasm> DisOrErr = Disasm::create(Img);
  if (!DisOrErr) {
    llvm::consumeError(DisOrErr.takeError());
    return {};
  }
  Disasm Dis = std::move(*DisOrErr);

  // Inline-data ranges (reloc slots, linker records) are boundaries too.
  std::vector<std::pair<uint64_t, uint64_t>> Extents;
  Extents.reserve(Chunks.size() + Img.InlineData.size());
  for (auto [A, B] : Chunks)
    Extents.emplace_back(A.Value, B.Value);
  for (const VaRange &R : Img.InlineData)
    Extents.emplace_back(R.Start.Value, R.End.Value);
  llvm::sort(Extents);
  std::vector<std::pair<uint64_t, uint64_t>> Merged;
  for (auto [A, B] : Extents) {
    if (!Merged.empty() && A <= Merged.back().second) {
      Merged.back().second = std::max(Merged.back().second, B);
      continue;
    }
    Merged.emplace_back(A, B);
  }

  // Bridge tiny filler-only cracks between data ranges: the compiler pads
  // merged table clusters with `int3`/`nop` bytes inside the data island.
  // A wide gap stays a gap.
  std::vector<std::pair<uint64_t, uint64_t>> Sorted;
  for (auto [A, B] : Data)
    Sorted.emplace_back(A.Value, B.Value);
  llvm::sort(Sorted);
  std::vector<std::pair<uint64_t, uint64_t>> Bridged;
  for (auto [A, B] : Sorted) {
    if (!Bridged.empty() && A > Bridged.back().second &&
        A - Bridged.back().second <= 16) {
      uint64_t P = Bridged.back().second;
      bool PureFiller = true;
      while (P < A) {
        auto I = Dis.decodeOne(Va(P));
        if (I && isFiller(*I, Img) && I->nextVa().Value <= A)
          P = I->nextVa().Value;
        else {
          PureFiller = false;
          break;
        }
      }
      if (PureFiller && P == A) {
        Bridged.back().second = B;
        continue;
      }
    }
    if (!Bridged.empty() && A <= Bridged.back().second) {
      Bridged.back().second = std::max(Bridged.back().second, B);
      continue;
    }
    Bridged.emplace_back(A, B);
  }

  std::vector<PaddingRun> Out;
  for (const RawSection &S : Pe.RawSections) {
    if (!S.Executable)
      continue;
    uint64_t DataLen = S.VirtualSize == 0
                           ? S.RawSize
                           : std::min(S.VirtualSize, S.RawSize);
    DataLen = std::min(DataLen, Pe.FileSize > S.FileOffset
                                      ? Pe.FileSize - S.FileOffset
                                      : 0);
    uint64_t VEnd = S.SecVa.Value + DataLen;
    if (VEnd < S.SecVa.Value)
      VEnd = UINT64_MAX;
    std::optional<uint64_t> Prev;
    for (auto [A0, B0] : Merged) {
      if (A0 >= VEnd)
        break;
      uint64_t A = std::max(A0, S.SecVa.Value);
      uint64_t B = std::min(B0, VEnd);
      if (A >= B)
        continue;
      if (Prev && *Prev < A) {
        // Split the gap at known data ranges; each remaining piece keeps
        // only its anchored filler runs.
        uint64_t Pos = *Prev;
        for (auto [DA, DB] : Bridged) {
          if (DB <= Pos)
            continue;
          if (DA >= A)
            break;
          if (DA > Pos)
            gapFillers(Dis, Img, Va(Pos), Va(DA), Out);
          Pos = std::max(Pos, DB);
          if (Pos >= A)
            break;
        }
        if (Pos < A)
          gapFillers(Dis, Img, Va(Pos), Va(A), Out);
      }
      // Advance past the whole gap, not just the filler prefix.
      Prev = !Prev ? B : std::max(*Prev, B);
    }
    // The mapped tail after the last claimed chunk, same anchored rules,
    // still split at known data ranges.
    if (Prev && *Prev < VEnd) {
      uint64_t Pos = *Prev;
      for (auto [DA, DB] : Bridged) {
        if (DB <= Pos)
          continue;
        if (DA >= VEnd)
          break;
        if (DA > Pos)
          gapFillers(Dis, Img, Va(Pos), Va(DA), Out);
        Pos = std::max(Pos, DB);
        if (Pos >= VEnd)
          break;
      }
      if (Pos < VEnd)
        gapFillers(Dis, Img, Va(Pos), Va(VEnd), Out);
    }
  }
  llvm::sort(Out, [](const PaddingRun &A, const PaddingRun &B) {
    return A.Start < B.Start;
  });
  Out.erase(std::unique(Out.begin(), Out.end(),
                        [](const PaddingRun &A, const PaddingRun &B) {
                          return A.Start == B.Start && A.End == B.End;
                        }),
            Out.end());
  return Out;
}

} // end namespace revenant
