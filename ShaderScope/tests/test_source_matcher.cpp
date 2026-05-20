#include <gtest/gtest.h>
#include "util/SourceMatcher.h"
#include "util/SourceInfo.h"

static std::vector<SourceInfo> threeSources() {
    return {
        { "monitor:root",  "Monitor: full root (3840x2160)" },
        { "monitor:DP-1",  "Monitor: DP-1 (1920x1080)" },
        { "window:0xabcd", "Window: Firefox (firefox-esr)" },
    };
}

TEST(SourceMatcher, ExactIdHitsRegardlessOfSubstring) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "monitor:root", picked));
    EXPECT_EQ("monitor:root", picked.id);
}

TEST(SourceMatcher, UniqueSubstringMatchesDisplayName) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "DP-1", picked));
    EXPECT_EQ("monitor:DP-1", picked.id);
}

TEST(SourceMatcher, SubstringIsCaseInsensitive) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "firefox", picked));
    EXPECT_EQ("window:0xabcd", picked.id);

    EXPECT_EQ(SourceMatchResult::Picked, matchSource(srcs, "FIREFOX", picked));
    EXPECT_EQ("window:0xabcd", picked.id);
}

TEST(SourceMatcher, AmbiguousSubstringMatchesMultiple) {
    std::vector<SourceInfo> srcs = {
        { "window:0x1", "Window: Firefox A (firefox)" },
        { "window:0x2", "Window: Firefox B (firefox)" },
    };
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::Ambiguous, matchSource(srcs, "firefox", picked));
}

TEST(SourceMatcher, NoMatchReturnsNoMatch) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::NoMatch, matchSource(srcs, "bogus-12345", picked));
}

TEST(SourceMatcher, EmptyQueryIsNoMatch) {
    auto srcs = threeSources();
    SourceInfo picked;
    EXPECT_EQ(SourceMatchResult::NoMatch, matchSource(srcs, "", picked));
}
