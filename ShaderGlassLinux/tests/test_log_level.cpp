// Unit tests for the LogLevel parser and threshold env-var resolution.
// No live output is captured — we only verify the filter decisions.

#include <gtest/gtest.h>
#include "util/Logging.h"
#include <cstdlib>

namespace LD = LoggingDetail;

TEST(LogLevel, ParseRecognisesCanonicalNames) {
    EXPECT_EQ(LogLevel::Debug, LD::parse("debug", LogLevel::Info));
    EXPECT_EQ(LogLevel::Info,  LD::parse("info",  LogLevel::Info));
    EXPECT_EQ(LogLevel::Warn,  LD::parse("warn",  LogLevel::Info));
    EXPECT_EQ(LogLevel::Error, LD::parse("error", LogLevel::Info));
    EXPECT_EQ(LogLevel::Off,   LD::parse("off",   LogLevel::Info));
}

TEST(LogLevel, ParseAcceptsCommonSynonyms) {
    EXPECT_EQ(LogLevel::Warn,  LD::parse("warning", LogLevel::Info));
    EXPECT_EQ(LogLevel::Error, LD::parse("err",     LogLevel::Info));
    EXPECT_EQ(LogLevel::Off,   LD::parse("none",    LogLevel::Info));
    EXPECT_EQ(LogLevel::Off,   LD::parse("silent",  LogLevel::Info));
}

TEST(LogLevel, ParseIsCaseInsensitive) {
    EXPECT_EQ(LogLevel::Debug, LD::parse("DEBUG",   LogLevel::Info));
    EXPECT_EQ(LogLevel::Warn,  LD::parse("Warning", LogLevel::Info));
    EXPECT_EQ(LogLevel::Error, LD::parse("ErRoR",   LogLevel::Info));
}

TEST(LogLevel, ParseFallsBackForUnknownOrEmpty) {
    EXPECT_EQ(LogLevel::Warn, LD::parse("verbose", LogLevel::Warn));
    EXPECT_EQ(LogLevel::Info, LD::parse("",        LogLevel::Info));
    EXPECT_EQ(LogLevel::Info, LD::parse(nullptr,   LogLevel::Info));
}

TEST(LogLevel, ShouldLogRespectsOrdering) {
    LD::resetThresholdForTesting();
    ::setenv("SHADERGLASS_LOG", "warn", 1);
    LD::resetThresholdForTesting();

    EXPECT_FALSE(LD::shouldLog(LogLevel::Debug));
    EXPECT_FALSE(LD::shouldLog(LogLevel::Info));
    EXPECT_TRUE (LD::shouldLog(LogLevel::Warn));
    EXPECT_TRUE (LD::shouldLog(LogLevel::Error));

    ::unsetenv("SHADERGLASS_LOG");
    LD::resetThresholdForTesting();
}

TEST(LogLevel, OffSuppressesEverything) {
    ::setenv("SHADERGLASS_LOG", "off", 1);
    LD::resetThresholdForTesting();

    EXPECT_FALSE(LD::shouldLog(LogLevel::Debug));
    EXPECT_FALSE(LD::shouldLog(LogLevel::Info));
    EXPECT_FALSE(LD::shouldLog(LogLevel::Warn));
    EXPECT_FALSE(LD::shouldLog(LogLevel::Error));

    ::unsetenv("SHADERGLASS_LOG");
    LD::resetThresholdForTesting();
}

TEST(LogLevel, DefaultThresholdIsInfo) {
    ::unsetenv("SHADERGLASS_LOG");
    LD::resetThresholdForTesting();
    EXPECT_EQ(LogLevel::Info, LD::threshold());

    EXPECT_FALSE(LD::shouldLog(LogLevel::Debug));
    EXPECT_TRUE (LD::shouldLog(LogLevel::Info));
    EXPECT_TRUE (LD::shouldLog(LogLevel::Warn));
    EXPECT_TRUE (LD::shouldLog(LogLevel::Error));
}
