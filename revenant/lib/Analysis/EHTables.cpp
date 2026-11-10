//===- EHTables.cpp - PE exception metadata -------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/EHTables.h"
#include "llvm/ADT/SmallVector.h"
#include <algorithm>
#include <limits>

using namespace revenant;

namespace {
constexpr uint32_t MaxEntries = 4096;
constexpr uint32_t EH_MAGIC = 0x19930520;
constexpr uint32_t EH_MAGIC_1 = 0x19930521;
constexpr uint32_t EH_MAGIC_2 = 0x19930522;
constexpr uint32_t EH_MAGIC_3 = 0x19930523;
constexpr uint32_t EH_MAGIC_4 = 0x19930524;

std::optional<uint32_t> u32(const Image &I, Va A) { return I.readU32(A); }
std::optional<int32_t> i32(const Image &I, Va A) {
  auto V = u32(I, A);
  if (!V) return std::nullopt;
  return static_cast<int32_t>(*V);
}
std::optional<Va> rel32(const Image &I, Va Field) {
  auto D = i32(I, Field);
  if (!D) return std::nullopt;
  uint64_t Base = Field.Value + 4;
  if (*D < 0) {
    uint64_t N = static_cast<uint64_t>(-static_cast<int64_t>(*D));
    if (N > Base) return std::nullopt;
    return Va(Base - N);
  }
  uint64_t N = static_cast<uint32_t>(*D);
  if (N > std::numeric_limits<uint64_t>::max() - Base) return std::nullopt;
  return Va(Base + N);
}
std::optional<std::string> handlerImportName(const PeFile &PE, Va Handler) {
  const Image &I = *PE.Img;
  for (unsigned Depth = 0; Depth < 8; ++Depth) {
    auto B = I.dataAt(Handler, 9);
    if (B.size() >= 6 && B[0] == 0xff && B[1] == 0x25) {
      auto Slot = rel32(I, Handler.wrappingAdd(2));
      if (!Slot) return std::nullopt;
      for (const Import &Imp : PE.Imports)
        if (Imp.IatVa == *Slot && Imp.Name)
          return *Imp.Name;
      return std::nullopt;
    }
    if (B.size() >= 5 && B[0] == 0xe9) {
      auto Target = rel32(I, Handler.wrappingAdd(1));
      if (!Target || *Target == Handler) return std::nullopt;
      Handler = *Target;
      continue;
    }
    if (B.size() >= 7 && B[0] == 0x48 && B[1] == 0xff && B[2] == 0x25) {
      auto Slot = rel32(I, Handler.wrappingAdd(3));
      if (!Slot) return std::nullopt;
      for (const Import &Imp : PE.Imports)
        if (Imp.IatVa == *Slot && Imp.Name)
          return *Imp.Name;
      return std::nullopt;
    }
    if (B.size() >= 9 && B[0] == 0x48 && B[1] == 0x8b && B[2] == 0x05 &&
        B[7] == 0xff && B[8] == 0xe0) {
      auto Slot = rel32(I, Handler.wrappingAdd(3));
      if (!Slot) return std::nullopt;
      for (const Import &Imp : PE.Imports)
        if (Imp.IatVa == *Slot && Imp.Name)
          return *Imp.Name;
      return std::nullopt;
    }
    return std::nullopt;
  }
  return std::nullopt;
}

struct UnwindRecord { Va Handler; Va Data; uint8_t Flags; };
std::optional<UnwindRecord> readUnwind(const Image &I, const RuntimeFunction &R) {
  auto H = I.readAtMost(R.Unwind, 4);
  if (!H || H->size() != 4) return std::nullopt;
  uint8_t Flags = (*H)[0] >> 3;
  uint8_t Count = (*H)[2];
  uint64_t DataOff = 4 + ((Count + 1u) & ~1u) * 2u;
  if (Flags & 4) return std::nullopt; // CHAININFO has no language handler.
  auto HandlerRva = u32(I, R.Unwind.wrappingAdd(DataOff));
  if (!HandlerRva) return std::nullopt;
  Va Handler = I.vaOfRva(*HandlerRva);
  return UnwindRecord{Handler, R.Unwind.wrappingAdd(DataOff + 4), Flags};
}

std::optional<std::string> typeName(const Image &I, Va TypeDescriptor) {
  // MSVC TypeDescriptor: vftable, spare, then decorated NUL-terminated name.
  auto Name = I.readCstr(TypeDescriptor.wrappingAdd(16), 1024);
  if (Name && !Name->empty()) return Name;
  return std::nullopt;
}

bool bounded(const Image &I, Va A, uint64_t Size) {
  const Section *S = I.sectionOf(A);
  return S && A.Value <= std::numeric_limits<uint64_t>::max() - Size &&
         A.Value + Size <= S->end().Value && I.isBacked(A);
}

void readSEH(const Image &I, FunctionEH &F, Va Data) {
  auto Count = u32(I, Data);
  if (!Count || !*Count || *Count > MaxEntries ||
      !bounded(I, Data, 4 + uint64_t(*Count) * 16))
    return;
  Va E = Data.wrappingAdd(4);
  for (uint32_t N = 0; N < *Count; ++N, E = E.wrappingAdd(16)) {
    auto Begin = u32(I, E), End = u32(I, E.wrappingAdd(4));
    auto Filter = u32(I, E.wrappingAdd(8));
    auto Handler = u32(I, E.wrappingAdd(12));
    // Scope-table offsets are image RVAs. Begin/End must be ordered and
    // backed; the handler/filter RVAs only need to be mapped so synthetic
    // fixtures and linker tables without executable flags still decode.
    if (!Begin || !End || !Filter || !Handler || *End <= *Begin ||
        !I.sectionOf(I.vaOfRva(*Begin)) || !I.sectionOf(I.vaOfRva(*End)) ||
        !I.sectionOf(I.vaOfRva(*Handler)) ||
        (*Filter && !I.sectionOf(I.vaOfRva(*Filter))))
      return;
  }
  F.Model = EHModel::SEH;
  E = Data.wrappingAdd(4);
  for (uint32_t N = 0; N < *Count; ++N, E = E.wrappingAdd(16)) {
    Va Lo = I.vaOfRva(*u32(I, E));
    Va Hi = I.vaOfRva(*u32(I, E.wrappingAdd(4)));
    uint32_t Filter = *u32(I, E.wrappingAdd(8));
    Va Target = I.vaOfRva(*u32(I, E.wrappingAdd(12)));
    EHTryRegion T;
    T.Begin = Lo;
    T.End = Hi;
    T.State = static_cast<int32_t>(N);
    F.CallSites.push_back({Lo, static_cast<int32_t>(N)});
    if (!Filter) {
      T.Handlers.push_back(F.Handlers.size());
      F.Cleanups.push_back({Target, static_cast<int32_t>(N)});
      F.Handlers.push_back({Target, HandlerKind::Finally, {},
                            static_cast<int32_t>(N)});
    } else {
      F.Handlers.push_back({I.vaOfRva(Filter), HandlerKind::Filter, {},
                            static_cast<int32_t>(N)});
      T.Handlers.push_back(F.Handlers.size());
      F.Handlers.push_back({Target, HandlerKind::Catch, {},
                            static_cast<int32_t>(N)});
    }
    F.TryRegions.push_back(std::move(T));
  }
}

/// Resolve try-state intervals against the ordered MSVC IP-to-state map.
void resolveTryRanges(FunctionEH &F) {
  // FuncInfo may have unwind-map states but no catch blocks. Those states
  // still delimit calls that need cleanuppads for destructors/finally.
  for (const EHCleanup &Cleanup : F.Cleanups) {
    bool Covered = std::any_of(
        F.TryRegions.begin(), F.TryRegions.end(), [&](const EHTryRegion &T) {
          return Cleanup.State >= T.CatchLow && Cleanup.State <= T.CatchHigh;
        });
    if (Covered || Cleanup.State < 0)
      continue;
    EHTryRegion T;
    T.Begin = F.Function;
    T.End = F.End;
    T.State = Cleanup.State;
    T.CatchLow = Cleanup.State;
    T.CatchHigh = Cleanup.State;
    F.TryRegions.push_back(std::move(T));
  }
  std::vector<EHTryRegion> Resolved;
  for (const EHTryRegion &T : F.TryRegions) {
    bool Active = false;
    EHTryRegion Run = T;
    for (const EHCallSite &Site : F.CallSites) {
      bool InTry = Site.State >= T.CatchLow && Site.State <= T.CatchHigh;
      if (InTry) {
        if (!Active) {
          Run = T;
          Run.Begin = Site.Address;
          Active = true;
        }
        continue;
      }
      if (Active) {
        Run.End = Site.Address;
        if (Run.Begin < Run.End)
          Resolved.push_back(std::move(Run));
        Active = false;
      }
    }
    if (Active) {
      Run.End = F.End;
      if (Run.Begin < Run.End)
        Resolved.push_back(std::move(Run));
    }
  }
  F.TryRegions = std::move(Resolved);
}

bool isFuncInfoMagic(uint32_t M) {
  return M == EH_MAGIC || M == EH_MAGIC_1 || M == EH_MAGIC_2 ||
         M == EH_MAGIC_3 || M == EH_MAGIC_4;
}

/// The FuncInfo a __CxxFrameHandler3 handler-data block names. On x64 the
/// block holds the FuncInfo's RVA; an inline FuncInfo is the x86 layout.
std::optional<Va> funcInfo3At(const Image &I, Va Data) {
  auto First = u32(I, Data);
  if (!First)
    return std::nullopt;
  if (isFuncInfoMagic(*First))
    return Data;
  if (!*First)
    return std::nullopt;
  Va Info = I.vaOfRva(*First);
  if (auto Magic = u32(I, Info); Magic && isFuncInfoMagic(*Magic))
    return Info;
  return std::nullopt;
}

void readFuncInfo3(const Image &I, FunctionEH &F, Va HandlerData) {
  std::optional<Va> Info = funcInfo3At(I, HandlerData);
  if (!Info)
    return;
  Va Data = *Info;
  auto MaxState = i32(I, Data.wrappingAdd(4));
  auto UnwindRva = u32(I, Data.wrappingAdd(8));
  auto TryCount = u32(I, Data.wrappingAdd(12));
  auto TryRva = u32(I, Data.wrappingAdd(16));
  auto IPCount = u32(I, Data.wrappingAdd(20));
  auto IPRva = u32(I, Data.wrappingAdd(24));
  if (!MaxState || !UnwindRva || !TryCount || !TryRva || !IPCount || !IPRva ||
      *MaxState < -1 || *MaxState > static_cast<int32_t>(MaxEntries) ||
      *TryCount > MaxEntries || *IPCount > MaxEntries)
    return;
  F.Model = EHModel::MSVC;
  if (*UnwindRva) {
    Va U = I.vaOfRva(*UnwindRva);
    // The unwind map has one entry per state, MaxState entries in all.
    for (int32_t S = 0; S < *MaxState; ++S) {
      Va E = U.wrappingAdd(static_cast<uint64_t>(S) * 8);
      auto To = i32(I, E);
      auto Action = u32(I, E.wrappingAdd(4));
      if (!To || !Action)
        break;
      if (*Action)
        F.Cleanups.push_back({I.vaOfRva(*Action), S, *To});
    }
  }
  if (*TryRva) {
    Va T = I.vaOfRva(*TryRva);
    for (uint32_t N = 0; N < *TryCount; ++N, T = T.wrappingAdd(20)) {
      auto Lo = i32(I, T), Hi = i32(I, T.wrappingAdd(4));
      auto CatchHigh = i32(I, T.wrappingAdd(8));
      auto CatchCount = i32(I, T.wrappingAdd(12));
      auto CatchRva = u32(I, T.wrappingAdd(16));
      if (!Lo || !Hi || !CatchHigh || !CatchCount || !CatchRva ||
          *CatchCount < 0 || *CatchCount > static_cast<int32_t>(MaxEntries))
        break;
      EHTryRegion Region;
      Region.State = *Lo;
      Region.CatchLow = *Lo;
      Region.CatchHigh = *Hi;
      Region.Begin = F.Function;
      Region.End = F.End;
      if (*CatchRva) {
        Va C = I.vaOfRva(*CatchRva);
        for (int32_t J = 0; J < *CatchCount; ++J, C = C.wrappingAdd(20)) {
          auto TypeRva = u32(I, C.wrappingAdd(4));
          auto HandlerRva = u32(I, C.wrappingAdd(12));
          if (!TypeRva || !HandlerRva)
            break;
          std::string Name;
          std::optional<Va> TypeDescriptor;
          if (*TypeRva) {
            TypeDescriptor = I.vaOfRva(*TypeRva);
            if (auto Nme = typeName(I, *TypeDescriptor))
              Name = *Nme;
          }
          Region.Handlers.push_back(F.Handlers.size());
          if (*HandlerRva)
            F.Handlers.push_back({I.vaOfRva(*HandlerRva), HandlerKind::Catch,
                                  Name, *Lo, TypeDescriptor});
        }
      }
      F.TryRegions.push_back(std::move(Region));
    }
  }
  if (*IPRva) {
    Va IP = I.vaOfRva(*IPRva);
    for (uint32_t N = 0; N < *IPCount; ++N, IP = IP.wrappingAdd(8)) {
      auto Offset = u32(I, IP);
      auto State = i32(I, IP.wrappingAdd(4));
      if (!Offset || !State)
        break;
      // A parent and its funclets share one FuncInfo, and the IP map covers
      // all of them; each function keeps the entries inside its own code.
      Va Site = I.vaOfRva(*Offset);
      if (Site < F.Function || !(Site < F.End))
        continue;
      F.CallSites.push_back({Site, *State});
    }
  }
  resolveTryRanges(F);
}

std::optional<uint32_t> fh4int(const Image &I, Va &P) {
  auto First = I.dataAt(P, 1);
  if (First.empty())
    return std::nullopt;
  static constexpr uint8_t Lengths[16] = {1, 2, 1, 3, 1, 2, 1, 4,
                                          1, 2, 1, 3, 1, 2, 1, 5};
  uint8_t N = Lengths[First[0] & 0x0f];
  uint8_t Shift = N == 1 ? 1 : N == 2 ? 2 : N == 3 ? 3 : N == 4 ? 3 : 4;
  auto Bytes = I.read(P, N);
  if (!Bytes)
    return std::nullopt;
  uint32_t Value = (*Bytes)[0] >> Shift;
  unsigned Bit = 8 - Shift;
  for (uint8_t IByte = 1; IByte < N; ++IByte, Bit += 8)
    if (Bit < 32)
      Value |= uint32_t((*Bytes)[IByte]) << Bit;
  P = P.wrappingAdd(N);
  return Value;
}

std::optional<int32_t> relativeDword(const Image &I, Va &P) {
  auto V = i32(I, P);
  if (V)
    P = P.wrappingAdd(4);
  return V;
}

void readFuncInfo4(const Image &I, FunctionEH &F, Va Data) {
  auto HeaderBytes = I.dataAt(Data, 1);
  if (HeaderBytes.empty())
    return;
  if (HeaderBytes[0] & 0x80) {
    auto TargetRva = u32(I, Data);
    if (!TargetRva)
      return;
    Va IndirectInfo = I.vaOfRva(*TargetRva);
    if (!I.sectionOf(IndirectInfo) || IndirectInfo == Data)
      return;
    Data = IndirectInfo;
    HeaderBytes = I.dataAt(Data, 1);
    if (HeaderBytes.empty() || (HeaderBytes[0] & 0x80))
      return;
  }
  uint8_t Header = HeaderBytes[0];
  Va P = Data.wrappingAdd(1);
  if (Header & 0x04) {
    if (!fh4int(I, P)) return;
  }
  int32_t UnwindDisp = 0, TryDisp = 0;
  if (Header & 0x08) {
    auto V = relativeDword(I, P); if (!V) return;
    UnwindDisp = *V;
  }
  if (Header & 0x10) {
    auto V = relativeDword(I, P); if (!V) return;
    TryDisp = *V;
  }
  auto IPDisp = relativeDword(I, P);
  if (!IPDisp) return;
  if (Header & 0x01) {
    if (!fh4int(I, P)) return;
  }
  if (!(Header & 0x18)) return;
  if (UnwindDisp) {
    Va U(I.Base.Value + static_cast<int64_t>(UnwindDisp));
    Va Cursor = U;
    auto Count = fh4int(I, Cursor);
    if (Count && *Count <= MaxEntries) {
      for (uint32_t N = 0; N < *Count; ++N) {
        auto Packed = fh4int(I, Cursor);
        if (!Packed) break;
        uint32_t Kind = *Packed & 3;
        if (Kind == 3 || Kind == 1 || Kind == 2) {
          auto Action = relativeDword(I, Cursor);
          if (!Action) break;
          if (Kind != 3 && !fh4int(I, Cursor)) break;
          Va Target(I.Base.Value + *Action);
          if (I.sectionOf(Target))
            F.Cleanups.push_back({Target, static_cast<int32_t>(N)});
        }
      }
    }
  }
  if (TryDisp) {
    Va T(I.Base.Value + static_cast<int64_t>(TryDisp));
    auto Count = fh4int(I, T);
    if (!Count || *Count > MaxEntries) return;
    for (uint32_t N = 0; N < *Count; ++N) {
      auto Lo = fh4int(I, T), Hi = fh4int(I, T), CatchHi = fh4int(I, T);
      auto HandlerDisp = relativeDword(I, T);
      if (!Lo || !Hi || !CatchHi || !HandlerDisp) break;
      EHTryRegion Region;
      Region.State = static_cast<int32_t>(*Lo);
      Region.CatchLow = static_cast<int32_t>(*Lo);
      Region.CatchHigh = static_cast<int32_t>(*Hi);
      Va H(I.Base.Value + *HandlerDisp);
      auto HCount = fh4int(I, H);
      if (HCount && *HCount <= MaxEntries) {
        for (uint32_t J = 0; J < *HCount; ++J) {
          auto HB = I.dataAt(H, 1);
          if (HB.empty()) break;
          uint8_t Flags = HB[0]; H = H.wrappingAdd(1);
          if (Flags & 1) if (!fh4int(I, H)) break;
          std::string Name;
          std::optional<Va> TypeDescriptor;
          if (Flags & 2) {
            auto TypeDisp = relativeDword(I, H);
            if (!TypeDisp)
              break;
            TypeDescriptor = Va(I.Base.Value + *TypeDisp);
            if (auto TypeName = typeName(I, *TypeDescriptor))
              Name = *TypeName;
          }
          if (Flags & 4)
            if (!fh4int(I, H))
              break;
          auto Catch = relativeDword(I, H);
          if (!Catch)
            break;
          uint8_t ContinuationCount = (Flags >> 4) & 3;
          llvm::SmallVector<Va, 2> ContinuationVAs;
          for (uint8_t K = 0; K < ContinuationCount; ++K) {
            if (Flags & 8) {
              auto Continuation = relativeDword(I, H);
              if (!Continuation)
                break;
              ContinuationVAs.push_back(Va(I.Base.Value + *Continuation));
            } else {
              auto Continuation = fh4int(I, H);
              if (!Continuation)
                break;
              ContinuationVAs.push_back(
                  F.Function.wrappingAdd(*Continuation));
            }
          }
          if (*Catch) {
            Va Target(I.Base.Value + *Catch);
            if (I.sectionOf(Target)) {
              Region.Handlers.push_back(F.Handlers.size());
              F.Handlers.push_back({Target, HandlerKind::Catch, Name,
                                    static_cast<int32_t>(*Lo), TypeDescriptor,
                                    std::vector<Va>(ContinuationVAs.begin(),
                                                    ContinuationVAs.end())});
            }
          }
        }
      }
      F.TryRegions.push_back(std::move(Region));
    }
  }
  if ((Header & 0x02) && *IPDisp) {
    Va Segments(I.Base.Value + static_cast<int64_t>(*IPDisp));
    auto SegmentCount = fh4int(I, Segments);
    if (!SegmentCount || *SegmentCount > MaxEntries) return;
    uint32_t FunctionRva = static_cast<uint32_t>(F.Function.Value - I.Base.Value);
    int32_t SegmentIPDisp = 0;
    for (uint32_t N = 0; N < *SegmentCount; ++N) {
      auto StartRva = u32(I, Segments);
      auto Disp = relativeDword(I, Segments);
      if (!StartRva || !Disp) return;
      if (*StartRva == FunctionRva)
        SegmentIPDisp = *Disp;
    }
    IPDisp = SegmentIPDisp;
  }
  if (*IPDisp) {
    Va IP(I.Base.Value + static_cast<int64_t>(*IPDisp));
    auto Count = fh4int(I, IP);
    uint32_t Offset = 0;
    if (Count && *Count <= MaxEntries) {
      for (uint32_t N = 0; N < *Count; ++N) {
        auto Delta = fh4int(I, IP), State = fh4int(I, IP);
        if (!Delta || !State) break;
        Offset += *Delta;
        F.CallSites.push_back({F.Function.wrappingAdd(Offset),
                               static_cast<int32_t>(*State) - 1});
      }
    }
  }
  if (!F.TryRegions.empty() || !F.Cleanups.empty() || !F.CallSites.empty())
    F.Model = EHModel::MSVC;
  resolveTryRanges(F);
}

std::optional<uint64_t> uleb(const Image &I, Va &P) {
  uint64_t V = 0;
  for (unsigned Shift = 0; Shift < 64; Shift += 7) {
    auto B = I.dataAt(P, 1);
    if (B.empty())
      return std::nullopt;
    uint8_t C = B[0];
    P = P.wrappingAdd(1);
    V |= uint64_t(C & 0x7f) << Shift;
    if (!(C & 0x80))
      return V;
  }
  return std::nullopt;
}

std::optional<int64_t> sleb(const Image &I, Va &P) {
  int64_t V = 0;
  unsigned Shift = 0;
  uint8_t C = 0;
  do {
    auto B = I.dataAt(P, 1);
    if (B.empty() || Shift >= 64)
      return std::nullopt;
    C = B[0];
    P = P.wrappingAdd(1);
    V |= int64_t(C & 0x7f) << Shift;
    Shift += 7;
  } while (C & 0x80);
  if (Shift < 64 && (C & 0x40))
    V |= -(int64_t(1) << Shift);
  return V;
}

std::optional<uint64_t> encoded(const Image &I, Va &P, uint8_t Encoding,
                                Va FunctionBase) {
  if (Encoding == 0xff)
    return uint64_t(0);
  uint8_t Form = Encoding & 0x0f;
  Va Field = P;
  uint64_t Raw = 0;
  int64_t Signed = 0;
  switch (Form) {
  case 0x00:
  case 0x04: {
    auto V = I.readU64(P);
    if (!V) return std::nullopt;
    Raw = *V; P = P.wrappingAdd(8); break;
  }
  case 0x02:
  case 0x0a: {
    auto B = I.read(P, 2);
    if (!B) return std::nullopt;
    uint16_t V = uint16_t((*B)[0]) | (uint16_t((*B)[1]) << 8);
    if (Form == 0x02) Raw = V;
    else Signed = static_cast<int16_t>(V);
    P = P.wrappingAdd(2); break;
  }
  case 0x03: {
    auto V = u32(I, P); if (!V) return std::nullopt;
    Raw = *V; P = P.wrappingAdd(4); break;
  }
  case 0x0b: {
    auto V = i32(I, P); if (!V) return std::nullopt;
    Signed = *V; P = P.wrappingAdd(4); break;
  }
  case 0x01: {
    auto V = uleb(I, P); if (!V) return std::nullopt;
    Raw = *V; break;
  }
  case 0x09: {
    auto V = sleb(I, P); if (!V) return std::nullopt;
    Signed = *V; break;
  }
  case 0x0c: {
    auto V = I.readU64(P); if (!V) return std::nullopt;
    Signed = static_cast<int64_t>(*V); P = P.wrappingAdd(8); break;
  }
  default:
    return std::nullopt;
  }
  if (Form == 0x0a || Form == 0x0b || Form == 0x0c || Form == 0x09)
    Raw = static_cast<uint64_t>(Signed);
  uint8_t Application = Encoding & 0x70;
  if (Application == 0x10) {
    Raw = Field.Value + Raw;
  } else if (Application == 0x40) {
    Raw = FunctionBase.Value + Raw;
  } else if (Application != 0) {
    return std::nullopt;
  }
  if (Encoding & 0x80) {
    auto Pointer = I.readU64(Va(Raw));
    if (!Pointer) return std::nullopt;
    Raw = *Pointer;
  }
  return Raw;
}

std::optional<std::string> itaniumTypeName(const Image &I, Va TypeInfo) {
  auto NamePointer = I.readU64(TypeInfo.wrappingAdd(8));
  if (!NamePointer || !*NamePointer)
    return std::nullopt;
  return I.readCstr(Va(*NamePointer), 1024);
}

void readLSDA(const Image &I, FunctionEH &F, Va Data) {
  Va P = Data;
  auto LPEncoding = I.dataAt(P, 1);
  if (LPEncoding.empty()) return;
  uint8_t LPEnc = LPEncoding[0]; P = P.wrappingAdd(1);
  uint64_t LPStart = F.Function.Value;
  if (LPEnc != 0xff) {
    auto V = encoded(I, P, LPEnc, F.Function);
    if (!V) return;
    LPStart = *V;
  }
  auto TypeEncodingBytes = I.dataAt(P, 1);
  if (TypeEncodingBytes.empty()) return;
  uint8_t TypeEnc = TypeEncodingBytes[0]; P = P.wrappingAdd(1);
  uint64_t TypeTable = 0;
  if (TypeEnc != 0xff) {
    auto Offset = uleb(I, P);
    if (!Offset) return;
    TypeTable = P.Value + *Offset;
  }
  auto CallEncodingBytes = I.dataAt(P, 1);
  if (CallEncodingBytes.empty()) return;
  uint8_t CallEnc = CallEncodingBytes[0]; P = P.wrappingAdd(1);
  auto CallLength = uleb(I, P);
  if (!CallLength || !*CallLength || *CallLength > (1u << 20) ||
      !bounded(I, P, *CallLength))
    return;
  Va CallEnd = P.wrappingAdd(*CallLength);
  bool HasCallSite = false;
  while (P < CallEnd) {
    auto Start = encoded(I, P, CallEnc, F.Function);
    auto Length = encoded(I, P, CallEnc, F.Function);
    auto Landing = encoded(I, P, CallEnc, F.Function);
    auto Action = uleb(I, P);
    if (!Start || !Length || !Landing || !Action) break;
    if (!*Length) continue;
    uint64_t Lo = LPStart + *Start;
    uint64_t Hi = Lo + *Length;
    if (Hi <= Lo || Lo < F.Function.Value || Hi > F.End.Value) continue;
    HasCallSite = true;
    EHTryRegion Region;
    Region.Begin = Va(Lo); Region.End = Va(Hi);
    Region.State = static_cast<int32_t>(*Action);
    F.CallSites.push_back({Va(Lo), Region.State});
    if (*Landing) {
      Va LandingPad(LPStart + *Landing);
      Region.Handlers.push_back(F.Handlers.size());
      F.Handlers.push_back({LandingPad,
          *Action ? HandlerKind::Catch : HandlerKind::Cleanup, {}, Region.State});
      if (!*Action)
        F.Cleanups.push_back({LandingPad, Region.State});
      if (*Action && TypeTable) {
        Va A = CallEnd.wrappingAdd(*Action - 1);
        // Action records are 1-based signed type filters plus a signed
        // displacement relative to the following-offset field.
        for (unsigned N = 0; N < 64; ++N) {
          auto Filter = sleb(I, A);
          Va NextField = A;
          auto Next = sleb(I, A);
          if (!Filter || !Next) break;
          if (*Filter > 0) {
            uint8_t Form = TypeEnc & 0x0f;
            int64_t EntryOffset = -static_cast<int64_t>(*Filter) *
                                  ((Form == 0 || Form == 4) ? 8 : 4);
            Va TypeField(TypeTable + EntryOffset);
            if (auto Type = encoded(I, TypeField, TypeEnc, F.Function)) {
              F.Handlers.back().TypeDescriptor = Va(*Type);
              if (auto Name = itaniumTypeName(I, Va(*Type)))
                F.Handlers.back().TypeName = *Name;
            }
          }
          if (!*Next) break;
          A = NextField.wrappingAdd(static_cast<uint64_t>(*Next));
        }
      }
    }
    F.TryRegions.push_back(std::move(Region));
  }
  if (HasCallSite)
    F.Model = EHModel::Itanium;
}
} // namespace

std::vector<FunctionEH> revenant::decodeEHTables(const PeFile &PE) {
  std::vector<FunctionEH> Out;
  if (!PE.Img) return Out;
  const Image &I=*PE.Img;
  for (const RuntimeFunction &R : PE.Pdata) {
    if (R.ChainedTo) continue;
    auto U=readUnwind(I,R); if (!U) continue;
    FunctionEH F;
    F.Function = R.Begin;
    F.End = R.End;
    F.Personality = U->Handler;
    F.Metadata = U->Data;
    auto HandlerName = handlerImportName(PE, U->Handler);
    if (HandlerName) {
      if (HandlerName->find("C_specific_handler") != std::string::npos) {
        readSEH(I, F, U->Data);
      } else if (HandlerName->find("CxxFrameHandler3") != std::string::npos) {
        readFuncInfo3(I, F, U->Data);
      } else if (HandlerName->find("CxxFrameHandler4") != std::string::npos) {
        readFuncInfo4(I, F, U->Data);
      } else if (HandlerName->find("gxx_personality_seh0") != std::string::npos) {
        readLSDA(I, F, U->Data);
      } else if (HandlerName->find("rust_eh_personality") != std::string::npos) {
        readLSDA(I, F, U->Data);
        if (F.Model == EHModel::Itanium)
          F.Model = EHModel::Rust;
      }
    }
    if (F.Model == EHModel::None) {
      F.TryRegions.clear();
      F.Handlers.clear();
      F.Cleanups.clear();
      F.CallSites.clear();
    }
    if (F.Model == EHModel::None) {
      if (funcInfo3At(I, U->Data)) {
        readFuncInfo3(I, F, U->Data);
      } else {
        auto First = I.dataAt(U->Data, 1);
        if (First.empty())
          continue;
        uint8_t B = First[0];
        if (B == 0xff || B == 0x1b) {
          readLSDA(I, F, U->Data);
        } else if (B & 0x80) {
          auto TargetRva = u32(I, U->Data);
          Va Info = TargetRva ? I.vaOfRva(*TargetRva) : Va();
          auto Header = I.dataAt(Info, 1);
          if (!Header.empty() && !(Header[0] & 0x80) &&
              (Header[0] & 0x78))
            readFuncInfo4(I, F, U->Data);
          else
            readSEH(I, F, U->Data);
        } else if (B & 0x78) {
          readFuncInfo4(I, F, U->Data);
        } else {
          readSEH(I, F, U->Data);
        }
      }
    }
    if (F.Model != EHModel::None &&
        (!F.TryRegions.empty() || !F.Handlers.empty() ||
         !F.Cleanups.empty() || !F.CallSites.empty()))
      Out.push_back(std::move(F));
  }
  return Out;
}
