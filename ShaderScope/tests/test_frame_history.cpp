// ShaderDef.h (pulled in via Preset.h) expects these to be visible first —
// see the include-order gotcha in CLAUDE.md.
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <cstdint>
#include "render/VulkanContext.h"
#include "render/Texture.h"
#include "render/HeadlessOutput.h"
#include "render/Preset.h"

namespace fs = std::filesystem;

namespace {

// In-process multi-frame rig: the headless CLI renders exactly one frame
// per process, so history/feedback semantics (which need state across
// frames) are exercised by driving Preset + HeadlessOutput directly.
struct Rig {
    VulkanContext  ctx{{.headless = true, .enableValidation = true}};
    Texture        src{ctx, 4, 4, VK_FORMAT_R8G8B8A8_UNORM};
    HeadlessOutput out{ctx, 4, 4};

    void upload(uint8_t r, uint8_t g, uint8_t b) {
        std::vector<uint8_t> px(4 * 4 * 4);
        for (int i = 0; i < 16; ++i) {
            px[i * 4 + 0] = r;
            px[i * 4 + 1] = g;
            px[i * 4 + 2] = b;
            px[i * 4 + 3] = 255;
        }
        src.uploadFromCpu(px.data(), px.size(), /*srcStride*/ 4 * 4);
    }

    // One windowed-loop frame: advanceFrame() then render.
    std::vector<uint8_t> frame(Preset& preset) {
        preset.advanceFrame();
        return out.renderToBytes(src, preset);
    }
};

void expectColor(const std::vector<uint8_t>& img, uint8_t r, uint8_t g, uint8_t b,
                 const char* what) {
    ASSERT_GE(img.size(), size_t(4)) << what;
    for (int i = 0; i < 16; ++i) {
        EXPECT_NEAR(img[i * 4 + 0], r, 4) << what << " pixel " << i << " red";
        EXPECT_NEAR(img[i * 4 + 1], g, 4) << what << " pixel " << i << " green";
        EXPECT_NEAR(img[i * 4 + 2], b, 4) << what << " pixel " << i << " blue";
    }
}

} // namespace

TEST(FrameHistory, OriginalHistory1SamplesPreviousFrame) {
    Rig rig;
    Preset preset(rig.ctx, fs::path(TEST_DATA_DIR) / "history1.slangp", rig.out.format());
    preset.ensureSourceSize(4, 4, 4, 4);

    rig.upload(255, 0, 0);
    expectColor(rig.frame(preset), 0, 0, 0, "frame 1 (no history yet)");

    rig.upload(0, 255, 0);
    expectColor(rig.frame(preset), 255, 0, 0, "frame 2 (history1 = frame 1 input)");

    rig.upload(0, 0, 255);
    expectColor(rig.frame(preset), 0, 255, 0, "frame 3 (history1 = frame 2 input)");
}

TEST(FrameHistory, PassFeedbackSamplesPreviousFrameOutput) {
    // Pass 0 swaps red→green; the final pass shows PassFeedback0. A
    // degrade-to-original bug would render red instead of black/green.
    Rig rig;
    Preset preset(rig.ctx, fs::path(TEST_DATA_DIR) / "feedback.slangp", rig.out.format());
    preset.ensureSourceSize(4, 4, 4, 4);

    rig.upload(255, 0, 0);
    expectColor(rig.frame(preset), 0, 0, 0, "frame 1 (feedback starts black)");
    expectColor(rig.frame(preset), 0, 255, 0, "frame 2 (feedback = frame 1 pass-0 output)");
}

TEST(FrameHistory, PassOutputSamplesEarlierPassThisFrame) {
    // Pass 0 swaps red→green, pass 1 inverts, the final pass shows
    // PassOutput0 — green proves it read pass 0, not its inverted Source
    // (magenta) or the original input (red).
    Rig rig;
    Preset preset(rig.ctx, fs::path(TEST_DATA_DIR) / "passoutput.slangp", rig.out.format());
    preset.ensureSourceSize(4, 4, 4, 4);

    rig.upload(255, 0, 0);
    expectColor(rig.frame(preset), 0, 255, 0, "PassOutput0 = pass-0 output");
}

TEST(FrameHistory, AliasResolvesToPassOutput) {
    // alias0 = RefPass; the final pass samples "RefPass".
    Rig rig;
    Preset preset(rig.ctx, fs::path(TEST_DATA_DIR) / "alias.slangp", rig.out.format());
    preset.ensureSourceSize(4, 4, 4, 4);

    rig.upload(255, 0, 0);
    expectColor(rig.frame(preset), 0, 255, 0, "alias = pass-0 output");
}
