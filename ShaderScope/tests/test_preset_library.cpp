// PresetLibrary::scan() over a fixture tree with two categories.

#include <gtest/gtest.h>
#include "util/PresetLibrary.h"
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

TEST(PresetLibrary, ScansCategoriesFromImmediateSubdirs) {
    fs::path fixture = fs::path(TEST_DATA_DIR) / "preset_library_fixture";
    PresetLibrary lib;
    auto presets = lib.scanDir(fixture);

    ASSERT_EQ(presets.size(), 4u);

    // Sorted by (category, displayName)
    EXPECT_EQ(presets[0].category, "crt");
    EXPECT_EQ(presets[0].displayName, "crt-test");

    EXPECT_EQ(presets[1].category, "crt");
    EXPECT_EQ(presets[1].displayName, "scanline-test");

    EXPECT_EQ(presets[2].category, "upscale");
    EXPECT_EQ(presets[2].displayName, "jinc2-test");

    EXPECT_EQ(presets[3].category, "upscale");
    EXPECT_EQ(presets[3].displayName, "xbr-test");

    // Path round-trip
    for (const auto& p : presets) {
        EXPECT_TRUE(fs::exists(p.path));
        EXPECT_EQ(p.path.extension(), ".slangp");
    }
}

TEST(PresetLibrary, FlatDirYieldsStarterCategory) {
    fs::path tmp = fs::temp_directory_path() / "shaderscope_preset_flat_test";
    fs::create_directories(tmp);
    {
        std::ofstream o(tmp / "a.slangp"); o << "shaders = 1\n";
    }
    {
        std::ofstream o(tmp / "b.slangp"); o << "shaders = 1\n";
    }
    PresetLibrary lib;
    auto presets = lib.scanDir(tmp);
    fs::remove_all(tmp);

    ASSERT_EQ(presets.size(), 2u);
    EXPECT_EQ(presets[0].category, "starter");
    EXPECT_EQ(presets[1].category, "starter");
}

TEST(PresetLibrary, NonexistentDirYieldsEmptyList) {
    PresetLibrary lib;
    auto presets = lib.scanDir("/nonexistent/dir/that/should/not/exist");
    EXPECT_TRUE(presets.empty());
}
