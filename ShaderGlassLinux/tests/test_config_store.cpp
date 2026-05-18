// ConfigStore round-trip + atomic-write + malformed-file recovery.

#include <gtest/gtest.h>
#include "util/ConfigStore.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace {
fs::path tmpFile(const std::string& tag) {
    auto p = fs::temp_directory_path() / ("shaderglass_cfg_" + tag + ".json");
    fs::remove(p);
    return p;
}
} // namespace

TEST(ConfigStore, RoundTripsSourcePresetAndParams) {
    auto path = tmpFile("roundtrip");

    {
        ConfigStore cfg(path);
        cfg.setLastSource("x11-screen", "monitor:root");
        cfg.setLastPreset("crt-easymode.slangp");
        cfg.setPresetParams("crt-easymode.slangp",
            {{"scanline_strength", 0.8f}, {"curvature", 0.15f}});
        cfg.saveSync();
    }
    {
        ConfigStore cfg(path);
        cfg.load();
        auto s = cfg.lastSource();
        ASSERT_TRUE(s.has_value());
        EXPECT_EQ(s->kind, "x11-screen");
        EXPECT_EQ(s->id,   "monitor:root");
        EXPECT_EQ(cfg.lastPreset(), "crt-easymode.slangp");
        auto params = cfg.paramsFor("crt-easymode.slangp");
        ASSERT_EQ(params.size(), 2u);
        EXPECT_FLOAT_EQ(params["scanline_strength"], 0.8f);
        EXPECT_FLOAT_EQ(params["curvature"],         0.15f);
    }
}

TEST(ConfigStore, LoadOnMissingFileLeavesDefaults) {
    auto path = tmpFile("missing");
    ConfigStore cfg(path);
    cfg.load();
    EXPECT_FALSE(cfg.lastSource().has_value());
    EXPECT_TRUE (cfg.lastPreset().empty());
}

TEST(ConfigStore, LoadOnMalformedFileLeavesDefaults) {
    auto path = tmpFile("malformed");
    { std::ofstream o(path); o << "{ not real json !!! }"; }
    ConfigStore cfg(path);
    cfg.load();   // should not throw; should log + reset
    EXPECT_FALSE(cfg.lastSource().has_value());
    EXPECT_TRUE (cfg.lastPreset().empty());
}

TEST(ConfigStore, AtomicWriteSurvivesInterleavedReads) {
    auto path = tmpFile("atomic");
    ConfigStore cfg(path);
    cfg.setLastSource("x11-screen", "monitor:root");
    cfg.saveSync();
    // Save again — make sure the second save still produces a valid file
    cfg.setLastPreset("foo.slangp");
    cfg.saveSync();
    ConfigStore reader(path);
    reader.load();
    EXPECT_EQ(reader.lastPreset(), "foo.slangp");
}

TEST(ConfigStore, ResetWipesFileOnDisk) {
    auto path = tmpFile("reset");
    ConfigStore cfg(path);
    cfg.setLastPreset("foo.slangp");
    cfg.saveSync();
    cfg.resetAndDelete();
    EXPECT_FALSE(fs::exists(path));
}
