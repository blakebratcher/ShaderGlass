#include "SourceMatcher.h"
#include <algorithm>
#include <cctype>

static bool ci_contains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(),   needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

std::vector<SourceInfo> collectSubstringMatches(const std::vector<SourceInfo>& sources,
                                                const std::string& query) {
    std::vector<SourceInfo> hits;
    for (const auto& s : sources) {
        if (ci_contains(s.displayName, query)) hits.push_back(s);
    }
    return hits;
}

SourceMatchResult matchSource(const std::vector<SourceInfo>& sources,
                              const std::string& query,
                              SourceInfo& out) {
    if (query.empty()) return SourceMatchResult::NoMatch;
    for (const auto& s : sources) {
        if (s.id == query) { out = s; return SourceMatchResult::Picked; }
    }
    auto hits = collectSubstringMatches(sources, query);
    if (hits.size() == 1) { out = hits[0]; return SourceMatchResult::Picked; }
    if (hits.empty())     return SourceMatchResult::NoMatch;
    return SourceMatchResult::Ambiguous;
}
