//==- revenant/Analysis/Strings.h - Referenced string recovery -*- C++ -*-==//
//
// Collects printable strings at xreffed addresses. Mirrors
// crates/analysis/src/strings.rs: `collect` walks every xref target and keeps
// what `string_at` accepts — an ASCII run of >= 4 printable bytes or a UTF-16
// run of >= 4 non-control characters — read only from readable,
// non-executable sections.
//
//===----------------------------------------------------------------------===//

#ifndef REVENANT_ANALYSIS_STRINGS_H
#define REVENANT_ANALYSIS_STRINGS_H

#include "revenant/Core/Image.h"
#include "revenant/Core/Program.h"
#include "revenant/Core/Va.h"

#include <map>
#include <optional>
#include <string>

namespace revenant {

/// Strings referenced from code: xref target VA -> text.
std::map<Va, std::string> collectStrings(const Image &Img, const Xrefs &Xr);

/// Try to read a printable string (ASCII >= 4 chars or UTF-16 >= 4 chars) at
/// `Addr`. Only reads from non-executable, readable sections.
std::optional<std::string> stringAt(const Image &Img, Va Addr);

} // end namespace revenant

#endif // REVENANT_ANALYSIS_STRINGS_H
