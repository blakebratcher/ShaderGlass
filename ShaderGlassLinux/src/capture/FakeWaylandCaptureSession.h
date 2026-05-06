#pragma once
#include "WaylandCaptureSession.h"
#include <cstdint>
#include <vector>
#include <atomic>

class FakeWaylandCaptureSession : public WaylandCaptureSession {
public:
    // pixels: tightly-packed RGBA8, size = width*height*4. Copied internally.
    FakeWaylandCaptureSession(uint32_t width, uint32_t height,
                              std::vector<uint8_t> pixels,
                              uint32_t framesToEmit = 1);
    ~FakeWaylandCaptureSession() override;

    std::vector<SourceInfo> selectSource() override;
    void start(std::function<void(const CapturedFrame&)> onFrame) override;
    void releaseBuffer(void* sessionHandle) override;
    void stop() override;

    // Test-only - wait until all `framesToEmit` have been produced and released.
    void waitForCompletion();

private:
    uint32_t              m_width, m_height;
    std::vector<uint8_t>  m_pixels;
    uint32_t              m_framesToEmit;
    std::atomic<uint32_t> m_framesReleased{0};
    std::atomic<bool>     m_stopped{false};
};
