#include <gtest/gtest.h>
#include "util/ConfigStore.h"
#include <filesystem>
#include <fstream>

namespace {
std::filesystem::path tmpCfg(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("shaderglass-test-" + name);
    std::filesystem::remove(p);
    return p;
}
} // namespace

TEST(ConfigStoreCrop, SaveLoadRoundtrip) {
    auto p = tmpCfg("crop1");
    {
        ConfigStore cfg(p);
        cfg.setCropFor("x11-screen", "monitor:root", {120, 80, 600, 400});
        cfg.saveSync();
    }
    ConfigStore cfg(p);
    cfg.load();
    auto rect = cfg.cropFor("x11-screen", "monitor:root");
    ASSERT_TRUE(rect.has_value());
    EXPECT_EQ(rect->x, 120);
    EXPECT_EQ(rect->y, 80);
    EXPECT_EQ(rect->w, 600);
    EXPECT_EQ(rect->h, 400);
}

TEST(ConfigStoreCrop, MissingCropsKeyLoadsAsEmpty) {
    auto p = tmpCfg("crop2");
    {
        std::ofstream f(p);
        f << R"({"lastPreset":"foo.slangp"})";
    }
    ConfigStore cfg(p);
    cfg.load();
    EXPECT_FALSE(cfg.cropFor("x11-screen", "monitor:root").has_value());
    EXPECT_EQ(cfg.lastPreset(), "foo.slangp");
}

TEST(ConfigStoreCrop, KindNamespacesId) {
    auto p = tmpCfg("crop3");
    ConfigStore cfg(p);
    cfg.setCropFor("x11-screen",     "0x42", {1, 2, 3, 4});
    cfg.setCropFor("wayland-screen", "0x42", {10, 20, 30, 40});
    auto a = cfg.cropFor("x11-screen",     "0x42");
    auto b = cfg.cropFor("wayland-screen", "0x42");
    ASSERT_TRUE(a && b);
    EXPECT_EQ(a->w, 3);
    EXPECT_EQ(b->w, 30);
}

TEST(ConfigStoreCrop, ClearRemovesEntry) {
    auto p = tmpCfg("crop4");
    ConfigStore cfg(p);
    cfg.setCropFor("x11-screen", "monitor:root", {0, 0, 100, 100});
    EXPECT_TRUE(cfg.cropFor("x11-screen", "monitor:root").has_value());
    cfg.clearCropFor("x11-screen", "monitor:root");
    EXPECT_FALSE(cfg.cropFor("x11-screen", "monitor:root").has_value());
}
