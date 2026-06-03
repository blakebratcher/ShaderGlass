#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(EndToEnd, ShaderGCStockPresetMatchesReference) {
    fs::path bin    = fs::path(SHADERSCOPE_BIN);
    fs::path in     = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path preset = fs::path(TEST_DATA_DIR) / "stock.slangp";
    fs::path out    = fs::temp_directory_path() / "shaderscope_e2e_out.png";
    fs::path ref    = fs::path(TEST_DATA_DIR) / "reference_stock_4x4.png";
    fs::remove(out);

    std::string cmd = bin.string() + " --headless"
                    + " --preset " + preset.string()
                    + " --input "  + in.string()
                    + " --output " + out.string()
                    + " --width 4 --height 4";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "headless+preset run failed";
    ASSERT_TRUE(fs::exists(out));

    auto a = readFile(out);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size())) << "output PNG bytes differ from reference";
}

// ── Starter-preset rendering (semantic UBO + vertex-input shaders) ──────────
//
// Every preset under ShaderScope/shaders/starter/ follows the RetroArch
// convention: `in vec4 Position` / `in vec2 TexCoord` vertex inputs +
// `global.MVP * Position` + SourceSize/OutputSize/params in the UBO or
// push-constant block. These render all-black unless the runtime binds a
// quad VBO and writes the built-in semantics at their reflected offsets.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace {

struct DecodedPng {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // w*h*4
};

DecodedPng decodePng(const fs::path& p) {
    DecodedPng img;
    int channels = 0;
    unsigned char* data = stbi_load(p.string().c_str(), &img.w, &img.h, &channels, 4);
    if (data) {
        img.rgba.assign(data, data + size_t(img.w) * img.h * 4);
        stbi_image_free(data);
    }
    return img;
}

int runHeadless(const fs::path& preset, const fs::path& in, const fs::path& out,
                int w, int h) {
    fs::remove(out);
    std::string cmd = fs::path(SHADERSCOPE_BIN).string() + " --headless"
                    + " --preset " + preset.string()
                    + " --input "  + in.string()
                    + " --output " + out.string()
                    + " --width " + std::to_string(w)
                    + " --height " + std::to_string(h);
    return std::system(cmd.c_str());
}

} // namespace

TEST(EndToEndStarter, PassthroughPresetRendersInputUnchanged) {
    fs::path in     = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path preset = fs::path(STARTER_SHADERS_DIR) / "passthrough.slangp";
    fs::path out    = fs::temp_directory_path() / "shaderscope_e2e_starter_pass.png";

    ASSERT_EQ(runHeadless(preset, in, out, 4, 4), 0) << "headless run failed";
    ASSERT_TRUE(fs::exists(out));

    DecodedPng img = decodePng(out);
    ASSERT_EQ(img.w, 4);
    ASSERT_EQ(img.h, 4);
    // The starter passthrough must reproduce the input: every pixel red.
    for (int i = 0; i < img.w * img.h; ++i) {
        EXPECT_GT(img.rgba[i * 4 + 0], 250) << "pixel " << i << " red channel";
        EXPECT_LT(img.rgba[i * 4 + 1],   5) << "pixel " << i << " green channel";
        EXPECT_LT(img.rgba[i * 4 + 2],   5) << "pixel " << i << " blue channel";
    }
}

TEST(EndToEndStarter, CrtEasymodeRendersNonBlackOutput) {
    fs::path in     = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path preset = fs::path(STARTER_SHADERS_DIR) / "crt-easymode.slangp";
    fs::path out    = fs::temp_directory_path() / "shaderscope_e2e_starter_crt.png";

    ASSERT_EQ(runHeadless(preset, in, out, 64, 64), 0) << "headless run failed";
    ASSERT_TRUE(fs::exists(out));

    DecodedPng img = decodePng(out);
    ASSERT_EQ(img.w, 64);
    ASSERT_EQ(img.h, 64);
    // A CRT shader over a solid red input must produce predominantly
    // red-ish, clearly non-black output (scanlines may darken rows).
    int nonBlack = 0, reddish = 0;
    for (int i = 0; i < img.w * img.h; ++i) {
        const uint8_t r = img.rgba[i * 4 + 0];
        const uint8_t g = img.rgba[i * 4 + 1];
        const uint8_t b = img.rgba[i * 4 + 2];
        if (r > 10 || g > 10 || b > 10) nonBlack++;
        if (r > 60 && r > g && r > b)   reddish++;
    }
    EXPECT_GT(nonBlack, img.w * img.h / 4)
        << "output is (almost) all black — semantic UBO/vertex input regression";
    EXPECT_GT(reddish, img.w * img.h / 8)
        << "output lost the red input signal";
}

TEST(EndToEndStarter, Passthrough2PassPresetRendersInputUnchanged) {
    fs::path in     = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path preset = fs::path(STARTER_SHADERS_DIR) / "passthrough-2pass.slangp";
    fs::path out    = fs::temp_directory_path() / "shaderscope_e2e_starter_2pass.png";

    ASSERT_EQ(runHeadless(preset, in, out, 4, 4), 0) << "headless run failed";
    ASSERT_TRUE(fs::exists(out));

    DecodedPng img = decodePng(out);
    ASSERT_EQ(img.w, 4);
    ASSERT_EQ(img.h, 4);
    for (int i = 0; i < img.w * img.h; ++i) {
        EXPECT_GT(img.rgba[i * 4 + 0], 250) << "pixel " << i << " red channel";
        EXPECT_LT(img.rgba[i * 4 + 1],   5) << "pixel " << i << " green channel";
        EXPECT_LT(img.rgba[i * 4 + 2],   5) << "pixel " << i << " blue channel";
    }
}
