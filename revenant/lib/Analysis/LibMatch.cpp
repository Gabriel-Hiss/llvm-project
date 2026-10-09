//==- lib/Analysis/LibMatch.cpp - Static-library function matching --------==//
//
// Loads the deterministic libdb format, matches whole discovered functions
// after masking source-object relocations, and applies names/prototypes.
//
//===----------------------------------------------------------------------===//

#include "revenant/Analysis/LibMatch.h"

#include "revenant/Analysis/Signatures.h"
#include "revenant/Core/Image.h"
#include "X86BaseInfo.h"
#include "llvm/Support/FileSystem.h"
#include "revenant/Core/Program.h"

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace revenant {
namespace {

static Error invalidDB(StringRef Why) {
  return createStringError(inconvertibleErrorCode(), "invalid libdb: %s",
                           Why.str().c_str());
}

static std::optional<uint64_t> getUInt(const json::Object &O, StringRef Key) {
  auto V = O.getInteger(Key);
  if (!V || *V < 0)
    return std::nullopt;
  return static_cast<uint64_t>(*V);
}

static Expected<std::vector<uint8_t>> parseHexBytes(StringRef Hex) {
  if (Hex.size() % 2)
    return invalidDB("byte string has odd-length hex");
  std::vector<uint8_t> Bytes;
  Bytes.reserve(Hex.size() / 2);
  for (size_t I = 0; I < Hex.size(); I += 2) {
    unsigned V = 0;
    if (Hex.slice(I, I + 2).getAsInteger(16, V))
      return invalidDB("byte string contains non-hex characters");
    Bytes.push_back(static_cast<uint8_t>(V));
  }
  return Bytes;
}

static std::string hexBytes(ArrayRef<uint8_t> Bytes) {
  static const char Digits[] = "0123456789abcdef";
  std::string Result;
  Result.resize(Bytes.size() * 2);
  for (size_t I = 0; I < Bytes.size(); ++I) {
    Result[2 * I] = Digits[Bytes[I] >> 4];
    Result[2 * I + 1] = Digits[Bytes[I] & 0x0f];
  }
  return Result;
}

static json::Value prototypeToJSON(const LibraryPrototype &P) {
  json::Array Params;
  for (const LibraryParam &Param : P.Params) {
    json::Object Obj;
    Obj["name"] = Param.Name;
    Obj["type"] = Param.Type;
    Obj["bits"] = static_cast<int64_t>(Param.Bits);
    Obj["float"] = Param.Float;
    Params.push_back(std::move(Obj));
  }
  json::Object Obj;
  Obj["return_type"] = P.ReturnType;
  Obj["return_bits"] = static_cast<int64_t>(P.ReturnBits);
  Obj["return_float"] = P.ReturnFloat;
  Obj["return_void"] = P.ReturnVoid;
  Obj["varargs"] = P.VarArgs;
  Obj["params"] = std::move(Params);
  return Obj;
}

static Expected<LibraryPrototype> prototypeFromJSON(const json::Object &Obj) {
  LibraryPrototype P;
  auto ReturnType = Obj.getString("return_type");
  auto ReturnBits = getUInt(Obj, "return_bits");
  auto ReturnFloat = Obj.getBoolean("return_float");
  auto ReturnVoid = Obj.getBoolean("return_void");
  auto VarArgs = Obj.getBoolean("varargs");
  auto Params = Obj.getArray("params");
  if (!ReturnType || !ReturnBits || !ReturnFloat || !ReturnVoid || !VarArgs ||
      !Params)
    return invalidDB("prototype is missing required fields");
  P.ReturnType = ReturnType->str();
  P.ReturnBits = static_cast<uint32_t>(*ReturnBits);
  P.ReturnFloat = *ReturnFloat;
  P.ReturnVoid = *ReturnVoid;
  P.VarArgs = *VarArgs;
  for (const json::Value &Value : *Params) {
    const json::Object *ParamObj = Value.getAsObject();
    if (!ParamObj)
      return invalidDB("prototype parameter is not an object");
    auto Name = ParamObj->getString("name");
    auto Type = ParamObj->getString("type");
    auto Bits = getUInt(*ParamObj, "bits");
    auto Float = ParamObj->getBoolean("float");
    if (!Name || !Type || !Bits || !Float)
      return invalidDB("prototype parameter is missing required fields");
    P.Params.push_back({Name->str(), Type->str(),
                        static_cast<uint32_t>(*Bits), *Float});
  }
  return P;
}

static uint32_t registerForArg(unsigned Index, bool IsFloat) {
  static const uint32_t IntRegs[] = {X86::RCX, X86::RDX, X86::R8, X86::R9};
  static const uint32_t FloatRegs[] = {X86::XMM0, X86::XMM1, X86::XMM2,
                                       X86::XMM3};
  return IsFloat ? FloatRegs[Index] : IntRegs[Index];
}

/// Callee-name comparison key: imports come in as `dll!name`, archive
/// records store demangled `name` — compare on alphanumeric runs so
/// decoration spellings still score.
static std::string nameKey(StringRef Name) {
  size_t Bang = Name.rfind('!');
  if (Bang != StringRef::npos)
    Name = Name.drop_front(Bang + 1);
  std::string Key;
  for (char C : Name)
    if (isAlnum(C) || C == '_')
      Key.push_back(C);
  return Key;
}

static uint32_t parseTypeBits(StringRef Type, uint32_t Fallback) {
  Type = Type.trim();
  if (Type.contains("char") || Type.contains("bool"))
    return 8;
  if (Type.contains("short") || Type.contains("wchar_t"))
    return 16;
  if (Type.contains("float"))
    return 32;
  if (Type.contains("double"))
    return 64;
  if (Type.contains("long long") || Type.contains("__int64"))
    return 64;
  if (Type.contains("long") || Type.contains("int") ||
      Type.contains("enum"))
    return 32;
  if (Type.contains("*") || Type.contains("&"))
    return 64;
  return Fallback;
}

} // namespace

Expected<LibraryDatabase> LibraryDatabase::load(StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path);
  if (!Buffer)
    return createFileError(Path, Buffer.getError());
  Expected<json::Value> Root = json::parse((*Buffer)->getBuffer());
  if (!Root)
    return Root.takeError();
  const json::Object *Object = Root->getAsObject();
  if (!Object)
    return invalidDB("root is not an object");
  auto Version = getUInt(*Object, "version");
  auto Entries = Object->getArray("functions");
  if (!Version || *Version != 1 || !Entries)
    return invalidDB("unsupported version or missing functions array");

  LibraryDatabase DB;
  for (const json::Value &Value : *Entries) {
    const json::Object *Entry = Value.getAsObject();
    if (!Entry)
      return invalidDB("function entry is not an object");
    auto Name = Entry->getString("name");
    auto MangledName = Entry->getString("mangled_name");
    auto Archive = Entry->getString("archive");
    auto Member = Entry->getString("member");
    auto Size = getUInt(*Entry, "size");
    auto BytesHex = Entry->getString("bytes");
    auto Mask = Entry->getArray("masked_relocations");
    auto Callees = Entry->getArray("callees");
    if (!Name || !MangledName || !Archive || !Member || !Size || !BytesHex ||
        !Mask || !Callees)
      return invalidDB("function entry is missing required fields");
    auto Bytes = parseHexBytes(*BytesHex);
    if (!Bytes)
      return Bytes.takeError();
    if (*Size != Bytes->size())
      return invalidDB("function size does not match byte string");
    LibraryFunction F;
    F.Name = Name->str();
    F.MangledName = MangledName->str();
    F.Member = Member->str();
    F.Size = *Size;
    F.Bytes = std::move(*Bytes);
    for (const json::Value &MaskValue : *Mask) {
      auto Offset = MaskValue.getAsInteger();
      if (!Offset || *Offset < 0 || static_cast<uint64_t>(*Offset) >= F.Size)
        return invalidDB("relocation mask offset is outside the function");
      F.MaskedRelocations.push_back(static_cast<uint32_t>(*Offset));
    }
    for (const json::Value &CalleeValue : *Callees) {
      auto Callee = CalleeValue.getAsString();
      if (!Callee)
        return invalidDB("callee name is not a string");
      F.Callees.push_back(Callee->str());
    }
    if (auto Prototype = Entry->getObject("prototype")) {
      auto Parsed = prototypeFromJSON(*Prototype);
      if (!Parsed)
        return Parsed.takeError();
      F.Prototype = std::move(*Parsed);
    }
    DB.Functions.push_back(std::move(F));
  }
  return DB;
}

Error LibraryDatabase::save(StringRef Path) const {
  json::Array Entries;
  for (const LibraryFunction &F : Functions) {
    json::Object Entry;
    Entry["name"] = F.Name;
    Entry["mangled_name"] = F.MangledName;
    Entry["archive"] = F.Archive;
    Entry["member"] = F.Member;
    Entry["size"] = static_cast<int64_t>(F.Size);
    Entry["bytes"] = hexBytes(F.Bytes);
    json::Array Mask;
    for (uint32_t Offset : F.MaskedRelocations)
      Mask.push_back(static_cast<int64_t>(Offset));
    Entry["masked_relocations"] = std::move(Mask);
    json::Array Callees;
    for (const std::string &Callee : F.Callees)
      Callees.push_back(Callee);
    Entry["callees"] = std::move(Callees);
    if (F.Prototype)
      Entry["prototype"] = prototypeToJSON(*F.Prototype);
    Entries.push_back(std::move(Entry));
  }
  json::Object Root;
  Root["version"] = 1;
  Root["functions"] = std::move(Entries);
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC)
    return createFileError(Path, EC);
  OS << formatv("{0:2}", json::Value(std::move(Root))) << '\n';
  return Error::success();
}

std::vector<LibraryMatch> matchLibraryFunctions(const Program &P,
                                                const LibraryDatabase &DB) {
  std::map<uint64_t, std::vector<uint32_t>> BySize;
  for (uint32_t I = 0; I < DB.Functions.size(); ++I)
    BySize[DB.Functions[I].Size].push_back(I);
  std::vector<LibraryMatch> Matches;
  if (!P.Img)
    return Matches;
  for (const auto &[Start, Record] : P.Functions) {
    if (Record.Chunks.size() != 1 || Record.Chunks[0].first != Start)
      continue;
    uint64_t Size = Record.Chunks[0].second - Start;
    if (Size > SIZE_MAX)
      continue;
    auto Raw = P.Img->dataAt(Start, static_cast<size_t>(Size));
    if (Raw.size() != Size)
      continue;
    for (auto It = BySize.lower_bound(Size);
         It != BySize.end() && It->first - Size <= 15; ++It) {
      for (uint32_t Index : It->second) {
        const LibraryFunction &F = DB.Functions[Index];
        if (F.Size != F.Bytes.size())
          continue;
        if (std::any_of(F.MaskedRelocations.begin(),
                        F.MaskedRelocations.end(),
                        [&](uint32_t Offset) { return Offset >= Size; }))
          continue;
        bool AlignedPadding = std::all_of(
            F.Bytes.begin() + Size, F.Bytes.end(),
            [](uint8_t Byte) { return Byte == 0x90 || Byte == 0xCC; });
        if (!AlignedPadding)
          continue;
        bool Equal = true;
        for (uint64_t I = 0; I < Size; ++I) {
          bool Masked = std::binary_search(F.MaskedRelocations.begin(),
                                           F.MaskedRelocations.end(),
                                           static_cast<uint32_t>(I));
          if (!Masked && Raw[I] != F.Bytes[I]) {
            Equal = false;
            break;
          }
        }
        if (Equal)
          Matches.push_back({Start, Index});
      }
    }
  }
  return Matches;
}

void applyLibraryMatches(Program &P, Signatures &Sigs,
                         ArrayRef<LibraryMatch> Matches,
                         const LibraryDatabase &DB) {
  if (!P.Names)
    P.Names = std::make_shared<std::map<Va, std::string>>();
  std::map<Va, std::vector<uint32_t>> ByStart;
  for (const LibraryMatch &M : Matches)
    if (M.FunctionIndex < DB.Functions.size())
      ByStart[M.Start].push_back(M.FunctionIndex);

  std::map<Va, uint32_t> Chosen;
  for (const auto &[Start, Candidates] : ByStart) {
    if (Candidates.size() == 1) {
      Chosen[Start] = Candidates.front();
      continue;
    }
    const std::string &Name = DB.Functions[Candidates.front()].Name;
    if (std::all_of(Candidates.begin(), Candidates.end(), [&](uint32_t I) {
          return DB.Functions[I].Name == Name;
        }))
      Chosen[Start] = Candidates.front();
  }

  bool Changed;
  do {
    Changed = false;
    for (const auto &[Start, Candidates] : ByStart) {
      if (Chosen.count(Start) || Candidates.size() < 2)
        continue;
      const FuncRecord *Record = nullptr;
      auto RecordIt = P.Functions.find(Start);
      if (RecordIt != P.Functions.end())
        Record = &RecordIt->second;
      if (!Record)
        continue;
      int BestScore = 0;
      std::optional<uint32_t> Best;
      bool Tie = false;
      for (uint32_t Candidate : Candidates) {
        const LibraryFunction &F = DB.Functions[Candidate];
        int Score = 0;
        std::set<std::string> Known;
        for (const std::string &Callee : F.Callees)
          Known.insert(nameKey(Callee));
        for (Va Callee : Record->CallTargets) {
          std::string Name;
          if (auto It = P.Names->find(Callee); It != P.Names->end())
            Name = It->second;
          else if (auto It = Chosen.find(Callee); It != Chosen.end())
            Name = DB.Functions[It->second].Name;
          if (!Name.empty() && Known.count(nameKey(Name)))
            ++Score;
        }
        if (Score > BestScore) {
          BestScore = Score;
          Best = Candidate;
          Tie = false;
        } else if (Score == BestScore && Score > 0) {
          Tie = true;
        }
      }
      if (Best && !Tie) {
        Chosen[Start] = *Best;
        Changed = true;
      }
    }
  } while (Changed);

  std::set<std::string> UsedNames;
  for (const auto &[Address, Name] : *P.Names)
    if (!Chosen.count(Address) || !StringRef(Name).starts_with("sub_"))
      UsedNames.insert(nameKey(Name));

  for (const auto &[Start, Index] : Chosen) {
    const LibraryFunction &F = DB.Functions[Index];
    auto Existing = P.Names->find(Start);
    bool ReplaceName = Existing == P.Names->end() ||
                       StringRef(Existing->second).starts_with("sub_");
    if (ReplaceName) {
      std::string Name = F.Name;
      if (UsedNames.count(nameKey(Name)))
        Name += "__" + utohexstr(Start.Value, true);
      UsedNames.insert(nameKey(Name));
      (*P.Names)[Start] = Name;
      auto RecordIt = P.Functions.find(Start);
      if (RecordIt != P.Functions.end())
        RecordIt->second.Name = std::move(Name);
    } else {
      UsedNames.insert(nameKey(Existing->second));
    }
    auto SigIt = Sigs.Funcs.find(Start);
    if (SigIt == Sigs.Funcs.end() || !F.Prototype)
      continue;
    Signature &Sig = SigIt->second;
    Sig.Params.clear();
    for (size_t I = 0; I < F.Prototype->Params.size(); ++I) {
      const LibraryParam &P_ = F.Prototype->Params[I];
      Param Param_;
      Param_.Bits = P_.Bits ? P_.Bits : parseTypeBits(P_.Type, 64);
      Param_.Float = P_.Float;
      Param_.Name = P_.Name;
      Param_.Type = P_.Type;
      if (I < 4)
        Param_.Loc = ParamLoc::reg(registerForArg(I, P_.Float));
      else
        Param_.Loc = ParamLoc::stack(0x28 + 8 * (I - 4));
      Sig.Params.push_back(std::move(Param_));
    }
    Sig.VarArgs = F.Prototype->VarArgs;
    Sig.HasRet = !F.Prototype->ReturnVoid;
    Sig.Ret.Float = F.Prototype->ReturnFloat;
    Sig.Ret.Bits = F.Prototype->ReturnBits;
    Sig.SigSource = Signature::Source::Library;
  }
  std::set<std::string> EmittedNames;
  for (const auto &[Address, Name] : *P.Names)
    if (!P.Functions.count(Address))
      EmittedNames.insert(nameKey(Name));
  for (auto &[Start, Record] : P.Functions) {
    std::string &Name = (*P.Names)[Start];
    if (Name.empty())
      Name = Record.Name;
    std::string Key = nameKey(Name);
    if (EmittedNames.count(Key)) {
      Name += "__" + utohexstr(Start.Value, true);
      Key = nameKey(Name);
    }
    EmittedNames.insert(Key);
    Record.Name = Name;
  }
}

} // namespace revenant
