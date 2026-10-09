//===- EHTables.h - PE exception metadata ---------------------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_EHTABLES_H
#define REVENANT_ANALYSIS_EHTABLES_H

#include "revenant/Object/PeFile.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace revenant {

enum class EHModel : uint8_t { None, MSVC, SEH, Itanium, Rust };
enum class HandlerKind : uint8_t { Catch, Filter, Finally, Cleanup };

struct EHTryRegion {
  Va Begin;
  Va End;
  int32_t State = -1;
  int32_t CatchHigh = -1;
  int32_t CatchLow = -1;
  std::vector<uint32_t> Handlers;
};

struct EHHandler {
  Va Address;
  HandlerKind Kind = HandlerKind::Catch;
  std::string TypeName;
  int32_t State = -1;
};

struct EHCleanup {
  Va Address;
  int32_t State = -1;
};

struct EHCallSite {
  Va Address;
  int32_t State = -1;
};

struct FunctionEH {
  Va Function;
  Va End;
  EHModel Model = EHModel::None;
  std::vector<EHTryRegion> TryRegions;
  std::vector<EHHandler> Handlers;
  std::vector<EHCleanup> Cleanups;
  std::vector<EHCallSite> CallSites;
  /// Runtime handler and metadata record, when the function uses one.
  std::optional<Va> Personality;
  std::optional<Va> Metadata;
};

/// Decode exception metadata reachable from the PE's .pdata records.
/// Malformed individual records are skipped; valid records remain available.
std::vector<FunctionEH> decodeEHTables(const PeFile &PE);

} // namespace revenant

#endif
