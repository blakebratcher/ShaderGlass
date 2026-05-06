#include <gtest/gtest.h>
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include <vector>
#include <cstdint>

// Allocate a 4x4 RGBA image, upload a known pattern, read it back via a
// staging buffer copy, and assert pixels match.
TEST(TextureUpload, RoundTrip4x4Rgba) {
    VulkanContext ctx({.headless = true, .enableValidation = true});

    std::vector<uint8_t> input(4 * 4 * 4);
    for (int i = 0; i < 16; ++i) {
        input[i*4 + 0] = (uint8_t)(i * 16);
        input[i*4 + 1] = (uint8_t)(255 - i * 16);
        input[i*4 + 2] = (uint8_t)i;
        input[i*4 + 3] = 255;
    }

    Texture tex(ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM);
    tex.uploadFromCpu(input.data(), input.size(), /*srcStride*/ 4 * 4);

    std::vector<uint8_t> output(4 * 4 * 4, 0);
    tex.downloadToCpu(output.data(), output.size(), /*dstStride*/ 4 * 4);

    EXPECT_EQ(output, input);
}
