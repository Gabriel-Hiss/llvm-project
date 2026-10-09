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
std::optional<Va> ptr32(const Image &I, Va Field) {
  auto V = u32(I, Field);
  if (!V || !*V) return std::nullopt;
  return I.vaOfRva(*V);
}
std::optional<Va> ptr64(const Image &I, Va Field) {
  auto V = I.readU64(Field);
  if (!V || !*V) return std::nullopt;
  return Va(*V);
}
bool bounded(const Image &I, Va A, uint64_t Size) {
  const Section *S = I.sectionOf(A);
  return S && A.Value <= std::numeric_limits<uint64_t>::max() - Size &&
         A.Value + Size <= S->end().Value && I.isBacked(A);
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

void readSEH(const Image &I, FunctionEH &F, Va Data) {
  auto Count = u32(I, Data);
  if (!Count || *Count > MaxEntries) return;
  F.Model = EHModel::SEH;
  Va E = Data.wrappingAdd(4);
  for (uint32_t N = 0; N < *Count; ++N, E = E.wrappingAdd(16)) {
    auto Begin = i32(I, E), End = i32(I, E.wrappingAdd(4));
    auto Filter = i32(I, E.wrappingAdd(8));
    auto Handler = i32(I, E.wrappingAdd(12));
    if (!Begin || !End || !Filter || !Handler) return;
    // Scope-table code offsets are image RVAs, not function-relative offsets.
    Va Lo = I.vaOfRva(static_cast<uint32_t>(*Begin));
    Va Hi = I.vaOfRva(static_cast<uint32_t>(*End));
    if (Hi <= Lo) continue;
    EHTryRegion T; T.Begin = Lo; T.End = Hi; T.State = static_cast<int32_t>(N);
    F.TryRegions.push_back(T);
    Va Target = I.vaOfRva(static_cast<uint32_t>(*Handler));
    if (*Filter == 0)
      F.Handlers.push_back({Target, HandlerKind::Finally, {}, static_cast<int32_t>(N)});
    else
      F.Handlers.push_back({I.vaOfRva(static_cast<uint32_t>(*Filter)),
                            HandlerKind::Filter, {}, static_cast<int32_t>(N)});
  }
}

void readFuncInfo3(const Image &I, FunctionEH &F, Va Data) {
  auto Magic = u32(I, Data);
  if (!Magic || (*Magic != EH_MAGIC && *Magic != EH_MAGIC_1 &&
                 *Magic != EH_MAGIC_2 && *Magic != EH_MAGIC_3))
    return;
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
    for (int32_t S = 0; S <= *MaxState; ++S) {
      Va E = U.wrappingAdd(static_cast<uint64_t>(S) * 8);
      auto To = i32(I, E);
      auto Action = u32(I, E.wrappingAdd(4));
      if (!To || !Action)
        break;
      if (*Action)
        F.Cleanups.push_back({I.vaOfRva(*Action), S});
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
          if (*TypeRva)
            if (auto Nme = typeName(I, I.vaOfRva(*TypeRva)))
              Name = *Nme;
          Region.Handlers.push_back(F.Handlers.size());
          if (*HandlerRva)
            F.Handlers.push_back({I.vaOfRva(*HandlerRva), HandlerKind::Catch,
                                  Name, *Lo});
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
      F.CallSites.push_back({I.vaOfRva(*Offset), *State});
    }
  }
  for (EHTryRegion &T : F.TryRegions) {
    for (size_t N = 0; N < F.CallSites.size(); ++N) {
      int32_t State = F.CallSites[N].State;
      if (State < T.CatchLow || State > T.CatchHigh)
        continue;
      T.Begin = F.CallSites[N].Address;
      T.End = N + 1 < F.CallSites.size() ? F.CallSites[N + 1].Address : F.End;
      break;
    }
  }
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
  if (HeaderBytes.empty() || (HeaderBytes[0] & 0x80))
    return;
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
  F.Model = EHModel::MSVC;
  if (UnwindDisp) {
    Va U(I.Base.Value + static_cast<int64_t>(UnwindDisp));
    Va Cursor = U;
    auto Count = fh4int(I, Cursor);
    if (Count && *Count <= MaxEntries) {
      for (uint32_t N = 0; N < *Count; ++N) {
        auto Packed = fh4int(I, Cursor);
        if (!Packed) break;
        uint32_t Kind = *Packed & 3;
        if (Kind == 3) {
          auto Action = relativeDword(I, Cursor);
          if (!Action) break;
          F.Cleanups.push_back({Va(I.Base.Value + *Action), static_cast<int32_t>(N)});
        } else if (Kind == 1 || Kind == 2) {
          auto Action = relativeDword(I, Cursor);
          auto Object = fh4int(I, Cursor);
          if (!Action || !Object) break;
          F.Cleanups.push_back({Va(I.Base.Value + *Action), static_cast<int32_t>(N)});
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
          if (Flags & 2) {
            auto TypeDisp = relativeDword(I, H);
            if (!TypeDisp) break;
            if (auto TypeName = typeName(I, Va(I.Base.Value + *TypeDisp)))
              Name = *TypeName;
          }
          if (Flags & 4) if (!fh4int(I, H)) break;
          auto Catch = relativeDword(I, H);
          if (!Catch) break;
          Region.Handlers.push_back(F.Handlers.size());
          if (*Catch)
            F.Handlers.push_back({Va(I.Base.Value + *Catch), HandlerKind::Catch,
                                  Name, static_cast<int32_t>(*Lo)});
          uint8_t Continuations = (Flags >> 4) & 3;
          for (uint8_t K = 0; K < Continuations; ++K) {
            if (Flags & 8) {
              if (!relativeDword(I, H)) break;
            } else if (!fh4int(I, H)) break;
          }
        }
      }
      F.TryRegions.push_back(std::move(Region));
    }
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
  for (EHTryRegion &T : F.TryRegions) {
    for (size_t N = 0; N < F.CallSites.size(); ++N) {
      int32_t State = F.CallSites[N].State;
      if (State < T.CatchLow || State > T.CatchHigh) continue;
      T.Begin = F.CallSites[N].Address;
      T.End = N + 1 < F.CallSites.size() ? F.CallSites[N + 1].Address : F.End;
      break;
    }
  }
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
  case 0x00: {
    auto V = I.readU64(P);
    if (!V) return std::nullopt;
    Raw = *V; P = P.wrappingAdd(8); break;
  }
  case 0x03: {
    auto V = u32(I, P);
    if (!V) return std::nullopt;
    Raw = *V; P = P.wrappingAdd(4); break;
  }
  case 0x0b: {
    auto V = i32(I, P);
    if (!V) return std::nullopt;
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
  default:
    return std::nullopt;
  }
  if (Form == 0x0b || Form == 0x09)
    Raw = static_cast<uint64_t>(Signed);
  uint8_t Application = Encoding & 0x70;
  if (Application == 0x10) {
    uint64_t Base = (Encoding & 0x80) ? FunctionBase.Value : Field.Value;
    return Base + Raw;
  }
  if (Application != 0)
    return std::nullopt;
  return Raw;
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
  if (!CallLength || *CallLength > (1u << 20)) return;
  Va CallEnd = P.wrappingAdd(*CallLength);
  F.Model = EHModel::Itanium;
  while (P < CallEnd) {
    auto Start = encoded(I, P, CallEnc, F.Function);
    auto Length = encoded(I, P, CallEnc, F.Function);
    auto Landing = encoded(I, P, CallEnc, F.Function);
    auto Action = uleb(I, P);
    if (!Start || !Length || !Landing || !Action) break;
    if (!*Length) continue;
    uint64_t Lo = LPStart + *Start;
    uint64_t Hi = Lo + *Length;
    if (Hi <= Lo) continue;
    EHTryRegion Region;
    Region.Begin = Va(Lo); Region.End = Va(Hi);
    Region.State = static_cast<int32_t>(*Action);
    if (*Landing) {
      Va LandingPad( LPStart + *Landing );
      Region.Handlers.push_back(F.Handlers.size());
      F.Handlers.push_back({LandingPad,
          *Action ? HandlerKind::Catch : HandlerKind::Cleanup, {}, Region.State});
      if (*Action && TypeTable) {
        Va A = CallEnd;
        // Action records are 1-based signed type filters plus a signed
        // displacement to the next action record.
        for (unsigned N = 0; N < 64; ++N) {
          auto Filter = sleb(I, A), Next = sleb(I, A);
          if (!Filter || !Next) break;
          if (*Filter > 0) {
            int64_t EntryOffset = -static_cast<int64_t>(*Filter) *
                                  ((TypeEnc & 0x0f) == 0x00 ? 8 : 4);
            Va TypeField(TypeTable + EntryOffset);
            if (auto Type = encoded(I, TypeField, TypeEnc, F.Function))
              if (auto Name = typeName(I, Va(*Type)))
                F.Handlers.back().TypeName = *Name;
          }
          if (!*Next) break;
          A = A.wrappingAdd(static_cast<uint64_t>(*Next));
        }
      }
    }
    F.TryRegions.push_back(std::move(Region));
  }
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
    auto Magic = u32(I, U->Data);
    if (Magic && (*Magic == EH_MAGIC || *Magic == EH_MAGIC_1 ||
                  *Magic == EH_MAGIC_2 || *Magic == EH_MAGIC_3)) {
      readFuncInfo3(I, F, U->Data);
    } else {
      auto First = I.dataAt(U->Data, 1);
      if (First.empty())
        continue;
      uint8_t B = First[0];
      if (B == 0xff || (B & 0x70) == 0x10) {
        readLSDA(I, F, U->Data);
      } else if (!(B & 0x80) && (B & 0x78)) {
        readFuncInfo4(I, F, U->Data);
      } else {
        readSEH(I, F, U->Data);
      }
    }
    if (F.Model != EHModel::None) Out.push_back(std::move(F));
  }
  return Out;
}
