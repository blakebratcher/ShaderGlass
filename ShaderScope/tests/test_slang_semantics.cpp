#include <gtest/gtest.h>
#include <map>
#include <string>
#include "render/SlangSemantics.h"

namespace {
const std::map<std::string, uint32_t> kNoAliases;
const std::map<std::string, uint32_t> kAliases = { {"RefPass", 1}, {"Blur", 3} };
}

TEST(SlangSemantics, ClassifiesCoreSamplerNames) {
    EXPECT_EQ(classifySamplerName("Source", kNoAliases).kind, SemanticTexKind::Source);
    EXPECT_EQ(classifySamplerName("Original", kNoAliases).kind, SemanticTexKind::Original);
    EXPECT_EQ(classifySamplerName("OriginalHistory0", kNoAliases).kind,
              SemanticTexKind::Original);
}

TEST(SlangSemantics, ClassifiesOriginalHistory) {
    auto r = classifySamplerName("OriginalHistory1", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::OriginalHistory);
    EXPECT_EQ(r.index, 1u);
    r = classifySamplerName("OriginalHistory12", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::OriginalHistory);
    EXPECT_EQ(r.index, 12u);
}

TEST(SlangSemantics, ClassifiesPassOutputAndFeedback) {
    auto r = classifySamplerName("PassOutput0", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassOutput);
    EXPECT_EQ(r.index, 0u);
    r = classifySamplerName("PassFeedback2", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassFeedback);
    EXPECT_EQ(r.index, 2u);
}

TEST(SlangSemantics, ResolvesAliases) {
    auto r = classifySamplerName("RefPass", kAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassOutput);
    EXPECT_EQ(r.index, 1u);
    r = classifySamplerName("BlurFeedback", kAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassFeedback);
    EXPECT_EQ(r.index, 3u);
}

TEST(SlangSemantics, RejectsNonSemanticNames) {
    EXPECT_EQ(classifySamplerName("BLUR_RADIUS", kNoAliases).kind, SemanticTexKind::Unknown);
    EXPECT_EQ(classifySamplerName("OriginalHistoryX", kNoAliases).kind, SemanticTexKind::Unknown);
    EXPECT_EQ(classifySamplerName("PassOutput", kNoAliases).kind, SemanticTexKind::Unknown);
    EXPECT_EQ(classifySamplerName("RefPass", kNoAliases).kind, SemanticTexKind::Unknown);
    EXPECT_EQ(classifySamplerName("Feedback", kAliases).kind, SemanticTexKind::Unknown);
}

TEST(SlangSemantics, ClassifiesSizeSemanticNames) {
    auto r = classifySizeSemanticName("OriginalHistorySize2", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::OriginalHistory);
    EXPECT_EQ(r.index, 2u);
    EXPECT_EQ(classifySizeSemanticName("OriginalHistorySize0", kNoAliases).kind,
              SemanticTexKind::Original);
    r = classifySizeSemanticName("PassOutputSize0", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassOutput);
    EXPECT_EQ(r.index, 0u);
    r = classifySizeSemanticName("PassFeedbackSize1", kNoAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassFeedback);
    EXPECT_EQ(r.index, 1u);
    r = classifySizeSemanticName("RefPassSize", kAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassOutput);
    EXPECT_EQ(r.index, 1u);
    r = classifySizeSemanticName("BlurFeedbackSize", kAliases);
    EXPECT_EQ(r.kind, SemanticTexKind::PassFeedback);
    EXPECT_EQ(r.index, 3u);
    EXPECT_EQ(classifySizeSemanticName("BLUR_RADIUS", kNoAliases).kind,
              SemanticTexKind::Unknown);
    EXPECT_EQ(classifySizeSemanticName("PassOutputSize", kNoAliases).kind,
              SemanticTexKind::Unknown);
}

TEST(SlangSemantics, IsIndexedSizeSemanticName) {
    EXPECT_TRUE(isIndexedSizeSemanticName("OriginalHistorySize3"));
    EXPECT_TRUE(isIndexedSizeSemanticName("PassOutputSize0"));
    EXPECT_TRUE(isIndexedSizeSemanticName("PassFeedbackSize11"));
    EXPECT_FALSE(isIndexedSizeSemanticName("SourceSize"));
    EXPECT_FALSE(isIndexedSizeSemanticName("OriginalSize"));
    EXPECT_FALSE(isIndexedSizeSemanticName("PassOutputSize"));
    EXPECT_FALSE(isIndexedSizeSemanticName("RefPassSize"));
}
