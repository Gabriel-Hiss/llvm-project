//==- revenant/Compiler/Profile.h - Compiler build traits --------*- C++ -*-==//
//
// Compiler identity and binary-visible build traits used by analyses.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_COMPILER_PROFILE_H
#define REVENANT_COMPILER_PROFILE_H

#include "revenant/Object/PeFile.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "llvm/ADT/StringRef.h"

namespace revenant {

enum class CompilerProducer : uint8_t { Unknown, Msvc, Clang, Gcc, Rustc, Go };
enum class SourceLanguage : uint8_t { C, Cpp, Rust, Go, Unknown };
enum class OptimizationLevel : uint8_t { Unknown, O0, O1, O2, O3, Os, Oz };
enum class ColdChunkConvention : uint8_t { None, ChainedUnwind, ColdSuffix, MfsSplit };
enum class EhModel : uint8_t { Unknown, MsvcFuncInfo, Seh, Lsda, RustCleanup };

/// Traits observable in the linked image. Unknown is distinct from false.
struct CompilerBuildTraits {
  OptimizationLevel Optimization = OptimizationLevel::Unknown;
  std::optional<bool> Lto;
  std::optional<bool> Pgo;
  std::optional<bool> StackProtection;
  std::optional<bool> ControlFlowGuard;
  std::optional<bool> FramePointers;
  bool HotColdSplit = false;
};

/// Inferred compiler and build profile. Detection never requires PDB data.
struct CompilerInfo {
  CompilerProducer Producer = CompilerProducer::Unknown;
  SourceLanguage Language = SourceLanguage::Unknown;
  std::string Version;
  CompilerBuildTraits Traits;
  std::vector<std::string> Evidence;
};

class CompilerProfile {
public:
  virtual ~CompilerProfile() = default;
  virtual CompilerProducer producer() const = 0;
  virtual CompilerBuildTraits traits() const = 0;
  virtual ColdChunkConvention coldChunkConvention() const { return ColdChunkConvention::None; }
  virtual EhModel ehModel() const { return EhModel::Unknown; }
  virtual bool isColdChunkName(llvm::StringRef Name) const;
};

class GenericLlvmProfile final : public CompilerProfile {
  CompilerBuildTraits Build;
public:
  explicit GenericLlvmProfile(CompilerBuildTraits T = {}) : Build(T) {}
  CompilerProducer producer() const override { return CompilerProducer::Unknown; }
  CompilerBuildTraits traits() const override { return Build; }
};

/// Detect producer and observable build traits from PE metadata, sections,
/// imports, symbols, and optional producer-specific evidence.
CompilerInfo detectCompiler(const PeFile &PE,
                            const std::vector<std::string> &Symbols = {});

/// Return a profile that owns no image memory. Unknown producers use the
/// generic LLVM-backend profile.
std::unique_ptr<CompilerProfile> profileFor(const PeFile &PE,
                                            const std::vector<std::string> &Symbols = {});

const char *producerName(CompilerProducer Producer);
const char *languageName(SourceLanguage Language);

} // namespace revenant

#endif // REVENANT_COMPILER_PROFILE_H
