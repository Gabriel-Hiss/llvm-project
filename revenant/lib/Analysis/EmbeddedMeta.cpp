//===- EmbeddedMeta.cpp - crash metadata in release images -----*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/EmbeddedMeta.h"
#include "revenant/Analysis/EHTables.h"
#include "revenant/Analysis/Analysis.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

using namespace llvm;

namespace revenant {
namespace {

constexpr size_t MaxLocationFile = 4096;
constexpr uint32_t MaxLocationValue = 1000000;

bool isRustSourcePath(StringRef Path) {
  return Path.ends_with(".rs") &&
         (Path.contains('/') || Path.contains('\\')) &&
         Path.size() <= MaxLocationFile;
}

bool readLocation(const Image &Img, Va Address, EmbeddedFunctionMetadata &Out) {
  auto FilePointer = Img.readU64(Address);
  auto FileLength = Img.readU64(Address.wrappingAdd(8));
  auto Line = Img.readU32(Address.wrappingAdd(16));
  auto Column = Img.readU32(Address.wrappingAdd(20));
  if (!FilePointer || !*FilePointer || !FileLength || !*FileLength ||
      *FileLength > MaxLocationFile || !Line || !*Line ||
      *Line > MaxLocationValue || !Column || !*Column ||
      *Column > MaxLocationValue)
    return false;

  auto Bytes = Img.read(Va(*FilePointer), static_cast<size_t>(*FileLength));
  if (!Bytes)
    return false;
  std::string Path(Bytes->begin(), Bytes->end());
  if (Path.find('\0') != std::string::npos || !isRustSourcePath(Path))
    return false;
  Out.SourceFile = std::move(Path);
  Out.Line = *Line;
  Out.Column = *Column;
  return true;
}

bool isPanicMessage(StringRef S) {
  return S.contains("panicked at") || S.contains("assertion failed") ||
         S.contains("called `Option::unwrap()") ||
         S.contains("called `Result::unwrap()") ||
         S.contains("index out of bounds") ||
         S.contains("capacity overflow") || S.contains("attempt to ");
}

bool isCrashMetadataString(StringRef S) {
  return S.contains("assertion") || S.contains("Assertion") ||
         S.contains("__FILE__") || S.contains("__FUNCTION__") ||
         S.ends_with(".c") || S.ends_with(".cc") || S.ends_with(".cpp") ||
         S.ends_with(".h") || S.ends_with(".hpp") || S.ends_with(".rs") ||
         S.starts_with("_ZN") || S.starts_with("?") || S.contains("::");
}

const FuncRecord *functionForReference(const Program &P, Va From) {
  return functionAt(P, From);
}

template <typename Fn>
void forEachXrefTo(const Xrefs &Refs, Va Target, Fn &&Visit) {
  auto Offset = Target.offsetFrom(Refs.Base);
  if (!Offset || *Offset > UINT32_MAX)
    return;
  auto It = std::lower_bound(Refs.Targets.begin(), Refs.Targets.end(),
                             static_cast<Rva>(*Offset));
  if (It == Refs.Targets.end() || *It != *Offset)
    return;
  size_t Group = static_cast<size_t>(It - Refs.Targets.begin());
  for (uint32_t I = Refs.Offsets[Group]; I < Refs.Offsets[Group + 1]; ++I)
    Visit(Xref{Va(Refs.Base.Value + Refs.Froms[I]), Refs.Kinds[I]});
}

void collectLocation(const Image &Img, Program &P, Va Target, Va From) {
  // The panic call loads the `Location` static itself (`lea rdx,[rip+loc]`),
  // so the referenced address is the record. Reading a record out of the
  // neighbourhood attached neighbouring constants to the wrong function: a
  // static that sits right before a record made the pass read that record.
  const FuncRecord *F = functionForReference(P, From);
  if (!F)
    return;
  EmbeddedFunctionMetadata M;
  if (!readLocation(Img, Target, M))
    return;
  auto [It, Inserted] = P.Embedded.emplace(F->Start, M);
  if (!Inserted && It->second.SourceFile.empty())
    It->second = std::move(M);
}

std::string classNameFromRTTI(StringRef Decorated) {
  if (!Decorated.starts_with(".?AV") && !Decorated.starts_with(".?AU"))
    return {};
  StringRef Name = Decorated.drop_front(4);
  size_t End = Name.find("@@");
  if (End == StringRef::npos || End == 0)
    return {};
  Name = Name.take_front(End);
  SmallVector<StringRef, 4> Parts;
  Name.split(Parts, '@');
  std::string Result;
  for (auto It = Parts.rbegin(); It != Parts.rend(); ++It) {
    if (It->empty())
      continue;
    if (!Result.empty())
      Result += "::";
    Result += It->str();
  }
  return Result;
}

void collectRtti(Program &P, const Image &Img) {
  std::map<Va, size_t> TypeByDescriptor;
  for (const Section &S : Img.Sections) {
    for (size_t Off = 16; Off + 8 < S.Data.size(); ++Off) {
      StringRef Tail(reinterpret_cast<const char *>(S.Data.data() + Off),
                     S.Data.size() - Off);
      if (!Tail.starts_with(".?AV") && !Tail.starts_with(".?AU"))
        continue;
      size_t End = Tail.find('\0');
      if (End == StringRef::npos || End > 1024)
        continue;
      StringRef Decorated = Tail.take_front(End);
      std::string Name = classNameFromRTTI(Decorated);
      if (Name.empty())
        continue;
      Va Descriptor = S.SecVa.wrappingAdd(Off - 16);
      auto Vftable = Img.readU64(Descriptor);
      auto Spare = Img.readU64(Descriptor.wrappingAdd(8));
      if (!Vftable || !*Vftable || !Spare || *Spare != 0)
        continue;
      TypeByDescriptor.emplace(Descriptor, P.EmbeddedTypes.size());
      P.EmbeddedTypes.push_back(
          {Descriptor, std::move(Name), Decorated.str(), Va(), {}});
    }
  }

  for (const Section &S : Img.Sections) {
    for (size_t Off = 0; Off + 24 <= S.Data.size(); Off += 4) {
      Va Locator = S.SecVa.wrappingAdd(Off);
      auto Signature = Img.readU32(Locator);
      auto TypeRva = Img.readU32(Locator.wrappingAdd(12));
      auto SelfRva = Img.readU32(Locator.wrappingAdd(20));
      if (!Signature || *Signature != 1 || !TypeRva || !SelfRva ||
          Img.vaOfRva(*SelfRva) != Locator)
        continue;
      Va Descriptor = Img.vaOfRva(*TypeRva);
      auto TypeIt = TypeByDescriptor.find(Descriptor);
      if (TypeIt == TypeByDescriptor.end())
        continue;
      EmbeddedTypeMetadata &Type = P.EmbeddedTypes[TypeIt->second];
      auto HierarchyRva = Img.readU32(Locator.wrappingAdd(16));
      if (HierarchyRva && *HierarchyRva) {
        Va Hierarchy = Img.vaOfRva(*HierarchyRva);
        auto Count = Img.readU32(Hierarchy.wrappingAdd(8));
        auto ArrayRva = Img.readU32(Hierarchy.wrappingAdd(12));
        if (Count && *Count <= 128 && ArrayRva && *ArrayRva) {
          Va BaseArray = Img.vaOfRva(*ArrayRva);
          for (uint32_t I = 0; I < *Count; ++I) {
            auto BaseDescriptorRva =
                Img.readU32(BaseArray.wrappingAdd(uint64_t(I) * 4));
            if (!BaseDescriptorRva || !*BaseDescriptorRva)
              break;
            Va BaseDescriptor = Img.vaOfRva(*BaseDescriptorRva);
            auto BaseTypeRva = Img.readU32(BaseDescriptor);
            if (!BaseTypeRva || !*BaseTypeRva)
              continue;
            auto BaseIt = TypeByDescriptor.find(Img.vaOfRva(*BaseTypeRva));
            if (BaseIt == TypeByDescriptor.end())
              continue;
            const std::string &BaseName =
                P.EmbeddedTypes[BaseIt->second].Name;
            if (BaseName != Type.Name &&
                std::find(Type.BaseClasses.begin(), Type.BaseClasses.end(),
                          BaseName) == Type.BaseClasses.end())
              Type.BaseClasses.push_back(BaseName);
          }
        }
      }
      for (const Section &V : Img.Sections) {
        for (size_t PtrOff = 0; PtrOff + 16 <= V.Data.size(); PtrOff += 8) {
          Va LocatorPointer = V.SecVa.wrappingAdd(PtrOff);
          auto LocatorValue = Img.readU64(LocatorPointer);
          if (!LocatorValue || *LocatorValue != Locator.Value)
            continue;
          Type.VTable = LocatorPointer.wrappingAdd(8);
          for (size_t MethodOff = PtrOff + 8;
               MethodOff + 8 <= V.Data.size() && MethodOff < PtrOff + 4096;
               MethodOff += 8) {
            auto Method = Img.readU64(V.SecVa.wrappingAdd(MethodOff));
            if (!Method || !Img.isExecutable(Va(*Method)))
              break;
            Va FunctionVa(*Method);
            Type.VirtualFunctions.push_back(FunctionVa);
            if (const FuncRecord *F = functionAt(P, FunctionVa)) {
              EmbeddedFunctionMetadata &Meta = P.Embedded[F->Start];
              if (std::find(Meta.ClassNames.begin(), Meta.ClassNames.end(),
                            Type.Name) == Meta.ClassNames.end())
                Meta.ClassNames.push_back(Type.Name);
            }
          }
        }
      }
    }
  }
  for (const EmbeddedTypeMetadata &Type : P.EmbeddedTypes)
    forEachXrefTo(P.Xr, Type.VTable, [&](const Xref &Ref) {
      const FuncRecord *F = functionForReference(P, Ref.From);
      if (!F)
        return;
      EmbeddedFunctionMetadata &Meta = P.Embedded[F->Start];
      if (std::find(Meta.ClassNames.begin(), Meta.ClassNames.end(),
                    Type.Name) == Meta.ClassNames.end())
        Meta.ClassNames.push_back(Type.Name);
    });
}


} // namespace

void applyEmbeddedMetadata(Program &P, const PeFile &PE) {
  P.Embedded.clear();
  if (!PE.Img)
    return;
  const Image &Img = *PE.Img;
  P.EmbeddedTypes.clear();
  collectRtti(P, Img);
  if (P.Names) {
    for (const CoffSymbol &Symbol : PE.CoffSymbols) {
      if (!Symbol.Address.Value || Symbol.Name.empty())
        continue;
      auto Function = P.Functions.find(Symbol.Address);
      if (Function != P.Functions.end()) {
        auto Existing = P.Names->find(Symbol.Address);
        if (Existing == P.Names->end() ||
            StringRef(Existing->second).starts_with("sub_")) {
          (*P.Names)[Symbol.Address] = Symbol.Name;
          Function->second.Name = Symbol.Name;
        }
      } else {
        P.Names->emplace(Symbol.Address, Symbol.Name);
      }
    }
  }

  for (const FunctionEH &EH : decodeEHTables(PE)) {
    for (const EHHandler &Handler : EH.Handlers) {
      if (Handler.TypeName.empty())
        continue;
      EmbeddedFunctionMetadata &M = P.Embedded[EH.Function];
      if (std::find(M.ExceptionTypes.begin(), M.ExceptionTypes.end(),
                    Handler.TypeName) == M.ExceptionTypes.end())
        M.ExceptionTypes.push_back(Handler.TypeName);
    }
  }

  for (Rva TargetRva : P.Xr.Targets) {
    Va Target = P.Xr.Base.wrappingAdd(TargetRva);
    forEachXrefTo(P.Xr, Target, [&](const Xref &Ref) {
      collectLocation(Img, P, Target, Ref.From);
    });
  }

  for (Rva TargetRva : P.Xr.Targets) {
    Va Target = P.Xr.Base.wrappingAdd(TargetRva);
    forEachXrefTo(P.Xr, Target, [&](const Xref &Ref) {
      if (P.Strings) {
        auto It = P.Strings->find(Target);
        if (It != P.Strings->end()) {
          const FuncRecord *F = functionForReference(P, Ref.From);
          if (F) {
            EmbeddedFunctionMetadata &M = P.Embedded[F->Start];
            if ((isPanicMessage(It->second) ||
                 (!M.SourceFile.empty() &&
                  !StringRef(It->second).ends_with(".rs"))) &&
                std::find(M.PanicMessages.begin(), M.PanicMessages.end(),
                          It->second) == M.PanicMessages.end())
              M.PanicMessages.push_back(It->second);
            if ((isCrashMetadataString(It->second) ||
                 It->second == F->Name) &&
                std::find(M.MetadataStrings.begin(), M.MetadataStrings.end(),
                          It->second) == M.MetadataStrings.end())
              M.MetadataStrings.push_back(It->second);
          }
        }
      }
    });
  }
}

} // namespace revenant
