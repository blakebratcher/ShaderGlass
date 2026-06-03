// Verifies X11Capture's frame-kind selection: it ships a CpuBuffer frame for
// a CPU X11SessionFrame, and a DmaBuf frame when the session populates
// importedDmaBuf. No X server or GPU required — uses an inline fake session.

#include <gtest/gtest.h>
#include "capture/X11Capture.h"
#include "capture/X11CaptureSession.h"
#include <memory>
#include <vector>
#include <cstdint>

namespace {

// Minimal X11CaptureSession that returns either a CPU or a DMA-BUF frame
// depending on construction.
class ProgrammableX11Session : public X11CaptureSession {
public:
    explicit ProgrammableX11Session(bool dmaBuf) : m_dmaBuf(dmaBuf) {
        m_pixels.assign(4 * 4 * 4, 0x55);
    }
    std::vector<SourceInfo> enumerateSources() override {
        return {{"monitor:root", "fake root"}};
    }
    void start(const SourceInfo&) override { m_started = true; }
    void stop() override { m_started = false; }
    std::optional<X11SessionFrame> grab() override {
        if (!m_started) return std::nullopt;
        X11SessionFrame f;
        f.width  = 4;
        f.height = 4;
        f.fourcc = 0x34325241;
        if (m_dmaBuf) {
            // The pointer just needs to be non-null for the kind decision; the
            // consumer treats it opaquely (would cast to ImportedDmaBuf*).
            f.importedDmaBuf = &m_fakeImport;
            f.modifier       = 0x123456;
        } else {
            f.data   = m_pixels.data();
            f.stride = 4 * 4;
        }
        return f;
    }
    std::pair<int,int> size() const override { return {4, 4}; }
private:
    bool                 m_dmaBuf;
    bool                 m_started = false;
    std::vector<uint8_t> m_pixels;
    int                  m_fakeImport = 0;  // stand-in handle
};

} // namespace

TEST(X11CaptureFrameKind, CpuSessionYieldsCpuBufferFrame) {
    X11Capture cap(std::make_unique<ProgrammableX11Session>(/*dmaBuf=*/false));
    cap.selectSource(cap.enumerateSources()[0]);
    auto f = cap.acquireFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(f->importedDmaBuf, nullptr);
    EXPECT_NE(f->data, nullptr);
    EXPECT_EQ(f->stride, 16u);
    cap.release(*f);
}

TEST(X11CaptureFrameKind, DmaBufSessionYieldsDmaBufFrame) {
    X11Capture cap(std::make_unique<ProgrammableX11Session>(/*dmaBuf=*/true));
    cap.selectSource(cap.enumerateSources()[0]);
    auto f = cap.acquireFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, CapturedFrame::Kind::DmaBuf);
    EXPECT_NE(f->importedDmaBuf, nullptr);
    EXPECT_EQ(f->data, nullptr);
    EXPECT_EQ(f->modifier, 0x123456u);
    cap.release(*f);
}

TEST(X11CaptureFrameKind, SetVulkanContextOnFakeSessionIsNoOp) {
    // Wiring a (null) Vulkan context onto a non-real session must be safe.
    X11Capture cap(std::make_unique<ProgrammableX11Session>(/*dmaBuf=*/false));
    cap.setVulkanContext(nullptr);  // dynamic_cast fails → no-op, no crash
    cap.selectSource(cap.enumerateSources()[0]);
    auto f = cap.acquireFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind, CapturedFrame::Kind::CpuBuffer);
    cap.release(*f);
}
