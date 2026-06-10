#pragma once
#include <cstdint>
#include <map>
#include <string>

// RetroArch slang semantic-texture name classification. Pure string logic —
// no Vulkan — so Preset's sampler routing and *Size semantic recognition can
// be unit-tested directly.
enum class SemanticTexKind {
    Source,           // "Source" — the previous pass's output (pass input)
    Original,         // "Original" / "OriginalHistory0" — the pass-0 input
    OriginalHistory,  // "OriginalHistory#" with # >= 1 — input # frames ago
    PassOutput,       // "PassOutput#" or a pass alias — pass # output, this frame
    PassFeedback,     // "PassFeedback#" / "<alias>Feedback" — pass # output, last frame
    Unknown,
};

struct SemanticTexRef {
    SemanticTexKind kind  = SemanticTexKind::Unknown;
    uint32_t        index = 0;  // history depth or pass number
};

// Classify a reflected sampler name. `aliasToPass` maps .slangp `aliasN`
// values to their pass index N.
SemanticTexRef classifySamplerName(const std::string& name,
                                   const std::map<std::string, uint32_t>& aliasToPass);

// Classify a UBO/push-constant member name of the *Size family:
// "OriginalHistorySize#", "PassOutputSize#", "PassFeedbackSize#",
// "<alias>Size", "<alias>FeedbackSize". Names already handled by the fixed
// semantic list (SourceSize, OriginalSize, …) are the caller's business.
SemanticTexRef classifySizeSemanticName(const std::string& name,
                                        const std::map<std::string, uint32_t>& aliasToPass);

// True for the numeric (alias-free) *Size# families only — usable from
// static contexts that have no alias map.
bool isIndexedSizeSemanticName(const std::string& name);
