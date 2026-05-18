// Drives X11Capture end-to-end against a FakeX11CaptureSession.
// Renders a known 4x4 BGRA buffer through the M1 headless pipeline and
// compares the output PNG against a committed reference.
//
// Mirrors test_wayland_capture_with_fake_session.cpp's structure.

#include <gtest/gtest.h>
#include "capture/X11Capture.h"
#include "capture/FakeX11CaptureSession.h"
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
#include "util/FourccToVk.h"
#include "builtin_shaders.h"
#include <stb_image_write.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(X11CaptureWithFakeSession, RendersBgraFrameToReferencePng) {
    // 4x4 red in BGRA layout: B=0, G=0, R=255, A=255.
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) {
        pixels[i*4 + 0] = 0;     // B
        pixels[i*4 + 1] = 0;     // G
        pixels[i*4 + 2] = 255;   // R
        pixels[i*4 + 3] = 255;   // A
    }

    auto session = std::make_unique<FakeX11CaptureSession>(4, 4, pixels);
    X11Capture cap(std::move(session));

    auto sources = cap.enumerateSources();
    ASSERT_GE(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(frame->width,  4u);
    EXPECT_EQ(frame->height, 4u);
    EXPECT_EQ(frame->fourcc, 0x34325241u);  // ARGB8888 (BGRA in memory)

    VulkanContext ctx({.headless = true, .enableValidation = true});

    VkFormat fmt = fourcc_to_vk(frame->fourcc);
    ASSERT_EQ(fmt, VK_FORMAT_B8G8R8A8_UNORM);

    Texture src(ctx, frame->width, frame->height, fmt);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    cap.release(*frame);

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        fmt);
    HeadlessOutput out(ctx, 4, 4, fmt);
    auto bytes = out.renderToBytes(src, pipeline);

    fs::path tmp = fs::temp_directory_path() / "shaderglass_x11_fake_out.png";
    fs::remove(tmp);
    ASSERT_TRUE(stbi_write_png(tmp.string().c_str(), 4, 4, 4,
                               bytes.data(), 4 * 4));

    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_x11_fake_4x4.png";
    auto a = readFile(tmp);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
