#include <gtest/gtest.h>
#include "util/ScreenshotWriter.h"
#include <stb_image.h>
#include <filesystem>
#include <vector>
#include <vulkan/vulkan.h>

namespace {
std::filesystem::path tmpFile(const std::string& name) {
    auto p = std::filesystem::temp_directory_path() / ("shaderglass-test-encode-" + name + ".png");
    std::filesystem::remove(p);
    return p;
}
} // namespace

TEST(ScreenshotEncode, WritesPngFromRgbaBuffer) {
    auto path = tmpFile("rgba");
    std::vector<uint8_t> pixels(64 * 32 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 255; pixels[i + 1] = 0; pixels[i + 2] = 0; pixels[i + 3] = 255;
    }

    ASSERT_TRUE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {64, 32}, VK_FORMAT_R8G8B8A8_UNORM));
    ASSERT_TRUE(std::filesystem::exists(path));

    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(w, 64);
    EXPECT_EQ(h, 32);
    EXPECT_EQ(data[0], 255);
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 255);
    stbi_image_free(data);
    std::filesystem::remove(path);
}

TEST(ScreenshotEncode, SwapsChannelsForBgra) {
    auto path = tmpFile("bgra");
    // Input is BGRA stored as (B=0, G=0, R=255, A=255). After the swap to
    // RGBA in the PNG, the decoded pixel should be (R=255, G=0, B=0, A=255).
    std::vector<uint8_t> pixels(8 * 8 * 4);
    for (size_t i = 0; i < pixels.size(); i += 4) {
        pixels[i + 0] = 0;   pixels[i + 1] = 0;
        pixels[i + 2] = 255; pixels[i + 3] = 255;
    }

    ASSERT_TRUE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {8, 8}, VK_FORMAT_B8G8R8A8_UNORM));

    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load(path.string().c_str(), &w, &h, &n, 4);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(data[0], 255);
    EXPECT_EQ(data[1], 0);
    EXPECT_EQ(data[2], 0);
    EXPECT_EQ(data[3], 255);
    stbi_image_free(data);
    std::filesystem::remove(path);
}

TEST(ScreenshotEncode, RejectsUnsupportedFormat) {
    auto path = tmpFile("rgb16");
    std::vector<uint8_t> pixels(4 * 4 * 4);
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, pixels.data(), {4, 4}, VK_FORMAT_R16G16B16A16_SFLOAT));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ScreenshotEncode, RejectsNullBufferAndZeroExtent) {
    auto path = tmpFile("null");
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, nullptr, {4, 4}, VK_FORMAT_R8G8B8A8_UNORM));
    std::vector<uint8_t> px(16);
    EXPECT_FALSE(ScreenshotWriter::encodeToPng(
        path, px.data(), {0, 4}, VK_FORMAT_R8G8B8A8_UNORM));
}
