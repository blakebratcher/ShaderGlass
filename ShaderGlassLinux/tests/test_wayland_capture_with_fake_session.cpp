// Drives WaylandCapture end-to-end against a FakeWaylandCaptureSession.
// Renders a known 4x4 RGBA buffer through the existing M1 headless pipeline
// and compares the output PNG against a committed reference.

#include <gtest/gtest.h>
#include "capture/WaylandCapture.h"
#include "capture/FakeWaylandCaptureSession.h"
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include "render/ShaderPipeline.h"
#include "render/HeadlessOutput.h"
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

TEST(WaylandCaptureWithFakeSession, RendersSyntheticFrameToReferencePng) {
    // 4x4 red, like the M1 reference fixture.
    std::vector<uint8_t> pixels(4 * 4 * 4);
    for (size_t i = 0; i < 16; ++i) {
        pixels[i*4 + 0] = 255;
        pixels[i*4 + 1] = 0;
        pixels[i*4 + 2] = 0;
        pixels[i*4 + 3] = 255;
    }
    auto session = std::make_unique<FakeWaylandCaptureSession>(4, 4, pixels, 1);
    auto* sessionRaw = session.get();
    WaylandCapture cap(std::move(session));
    auto sources = cap.enumerateSources();
    ASSERT_EQ(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto frame = cap.acquireFrame();
    ASSERT_TRUE(frame.has_value());
    EXPECT_EQ(frame->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(frame->width,  4u);
    EXPECT_EQ(frame->height, 4u);

    // Render through the existing headless pipeline.
    VulkanContext ctx({.headless = true, .enableValidation = true});
    Texture src(ctx, frame->width, frame->height, VK_FORMAT_R8G8B8A8_UNORM);
    src.uploadFromCpu(frame->data, frame->stride * frame->height, frame->stride);
    cap.release(*frame);
    sessionRaw->waitForCompletion();

    ShaderPipeline pipeline(ctx,
        g_passthrough_vert_spv, g_passthrough_vert_spv_len,
        g_passthrough_frag_spv, g_passthrough_frag_spv_len,
        VK_FORMAT_R8G8B8A8_UNORM);
    HeadlessOutput out(ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM);
    auto bytes = out.renderToBytes(src, pipeline);

    fs::path tmp = fs::temp_directory_path() / "shaderglass_fake_session_out.png";
    fs::remove(tmp);
    ASSERT_TRUE(stbi_write_png(tmp.string().c_str(), 4, 4, 4,
                               bytes.data(), 4 * 4));

    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_fake_session_4x4.png";
    auto a = readFile(tmp);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size()));
}
