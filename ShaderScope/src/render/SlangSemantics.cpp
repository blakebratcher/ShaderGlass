#include "SlangSemantics.h"
#include <cstring>

namespace {

// Matches `<prefix><digits>` exactly; writes the parsed index. Indices above
// 1000 are rejected — nothing legitimate gets near that.
bool splitIndexed(const std::string& name, const char* prefix, uint32_t& idx) {
    const size_t n = std::strlen(prefix);
    if (name.size() <= n || name.compare(0, n, prefix) != 0) return false;
    uint32_t v = 0;
    for (size_t i = n; i < name.size(); ++i) {
        const char c = name[i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + static_cast<uint32_t>(c - '0');
        if (v > 1000) return false;
    }
    idx = v;
    return true;
}

constexpr const char kFeedbackSuffix[] = "Feedback";
constexpr size_t     kFeedbackLen      = sizeof(kFeedbackSuffix) - 1;
constexpr const char kSizeSuffix[]     = "Size";
constexpr size_t     kSizeLen          = sizeof(kSizeSuffix) - 1;

} // namespace

SemanticTexRef classifySamplerName(const std::string& name,
                                   const std::map<std::string, uint32_t>& aliasToPass) {
    uint32_t idx = 0;
    if (name == "Source")   return { SemanticTexKind::Source, 0 };
    if (name == "Original") return { SemanticTexKind::Original, 0 };
    if (splitIndexed(name, "OriginalHistory", idx)) {
        return { idx == 0 ? SemanticTexKind::Original : SemanticTexKind::OriginalHistory, idx };
    }
    if (splitIndexed(name, "PassOutput", idx))   return { SemanticTexKind::PassOutput, idx };
    if (splitIndexed(name, "PassFeedback", idx)) return { SemanticTexKind::PassFeedback, idx };

    auto it = aliasToPass.find(name);
    if (it != aliasToPass.end()) return { SemanticTexKind::PassOutput, it->second };
    if (name.size() > kFeedbackLen
        && name.compare(name.size() - kFeedbackLen, kFeedbackLen, kFeedbackSuffix) == 0) {
        auto base = aliasToPass.find(name.substr(0, name.size() - kFeedbackLen));
        if (base != aliasToPass.end()) return { SemanticTexKind::PassFeedback, base->second };
    }
    return { SemanticTexKind::Unknown, 0 };
}

SemanticTexRef classifySizeSemanticName(const std::string& name,
                                        const std::map<std::string, uint32_t>& aliasToPass) {
    uint32_t idx = 0;
    if (splitIndexed(name, "OriginalHistorySize", idx)) {
        return { idx == 0 ? SemanticTexKind::Original : SemanticTexKind::OriginalHistory, idx };
    }
    if (splitIndexed(name, "PassOutputSize", idx))   return { SemanticTexKind::PassOutput, idx };
    if (splitIndexed(name, "PassFeedbackSize", idx)) return { SemanticTexKind::PassFeedback, idx };

    // Alias forms: "<alias>Size" / "<alias>FeedbackSize" — strip the Size
    // suffix and resolve the base like a sampler name (alias-only: the
    // numeric families were handled above, and "Source"/"Original" bases
    // belong to the caller's fixed semantic list).
    if (name.size() > kSizeLen
        && name.compare(name.size() - kSizeLen, kSizeLen, kSizeSuffix) == 0) {
        const std::string base = name.substr(0, name.size() - kSizeLen);
        const SemanticTexRef r = classifySamplerName(base, aliasToPass);
        if (r.kind == SemanticTexKind::PassOutput || r.kind == SemanticTexKind::PassFeedback) {
            return r;
        }
    }
    return { SemanticTexKind::Unknown, 0 };
}

bool isIndexedSizeSemanticName(const std::string& name) {
    uint32_t idx = 0;
    return splitIndexed(name, "OriginalHistorySize", idx)
        || splitIndexed(name, "PassOutputSize", idx)
        || splitIndexed(name, "PassFeedbackSize", idx);
}
