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

/// The runtime personality function the EH lift attaches for `Model` (empty
/// for `EHModel::None`). The name is a module-level global: the lift
/// creates the declaration lazily, and a lifted function can carry the same
/// name (a personality thunk a library match renamed after the runtime
/// symbol it forwards to). Whoever creates it first keeps the plain name and
/// the other is renamed to `<name>.decl`, so the lift order decides which
/// is which.
inline llvm::StringRef personalityName(EHModel Model) {
  switch (Model) {
  case EHModel::MSVC:
  case EHModel::Rust:
    return "__CxxFrameHandler3";
  case EHModel::SEH:
    return "__C_specific_handler";
  case EHModel::Itanium:
    return "__gxx_personality_seh0";
  case EHModel::None:
    break;
  }
  return "";
}

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
  /// RTTI descriptor VA used by native MSVC catchpad selectors.
  std::optional<Va> TypeDescriptor;
  /// Catch funclet continuation VAs encoded in FH4 handler metadata.
  std::vector<Va> Continuations;
};

struct EHCleanup {
  Va Address;
  int32_t State = -1;
  /// The state unwinding continues in once this cleanup ran (MSVC unwind
  /// map `toState`); -1 is the caller.
  int32_t ToState = -1;
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
