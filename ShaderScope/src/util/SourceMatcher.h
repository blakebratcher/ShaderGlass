#pragma once
#include "SourceInfo.h"
#include <string>
#include <vector>

enum class SourceMatchResult {
    Picked,      // exactly one source matched; `out` populated
    NoMatch,     // zero matches
    Ambiguous,   // more than one substring match
};

// Match `query` against `sources`. Algorithm:
//   1. If any source.id == query, pick that one (exact-id always wins).
//   2. Else collect sources whose displayName contains query (case-insensitive).
//   3. If exactly one, pick. If zero, NoMatch. If more, Ambiguous.
//
// Empty query -> NoMatch (caller is expected to handle the "omitted" case
// separately by checking before calling).
SourceMatchResult matchSource(const std::vector<SourceInfo>& sources,
                              const std::string& query,
                              SourceInfo& out);

// Convenience for diagnostics: collect display names matching the query.
// (Used when we want to print the ambiguous subset.)
std::vector<SourceInfo> collectSubstringMatches(const std::vector<SourceInfo>& sources,
                                                const std::string& query);
