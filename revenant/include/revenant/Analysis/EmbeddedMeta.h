//===- EmbeddedMeta.h - crash metadata in release images -------*- C++ -*-===//
//
// Part of the Revenant LLVM fork.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_EMBEDDEDMETA_H
#define REVENANT_ANALYSIS_EMBEDDEDMETA_H

#include "revenant/Core/Program.h"
#include "revenant/Object/PeFile.h"

namespace revenant {

/// Recover crash-reporting information retained by optimized binaries.
/// Results are descriptive only and must not affect discovery or control flow.
void applyEmbeddedMetadata(Program &P, const PeFile &PE);

} // namespace revenant

#endif
