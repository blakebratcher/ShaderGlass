#include "FakeWaylandCaptureSession.h"
#include <stdexcept>

FakeWaylandCaptureSession::FakeWaylandCaptureSession(
    uint32_t width, uint32_t height,
    std::vector<uint8_t> pixels, uint32_t framesToEmit)
    : m_width(width), m_height(height),
      m_pixels(std::move(pixels)), m_framesToEmit(framesToEmit) {
    if (m_pixels.size() != size_t(width) * height * 4) {
        throw std::invalid_argument("FakeWaylandCaptureSession: pixels size mismatch");
    }
}

FakeWaylandCaptureSession::~FakeWaylandCaptureSession() = default;

std::vector<SourceInfo> FakeWaylandCaptureSession::selectSource() {
    return { { "fake://0", "fake source" } };
}

void FakeWaylandCaptureSession::start(std::function<void(const CapturedFrame&)> onFrame) {
    for (uint32_t i = 0; i < m_framesToEmit; ++i) {
        if (m_stopped.load()) break;
        CapturedFrame f;
        f.kind          = CapturedFrame::Kind::CpuBuffer;
        f.width         = m_width;
        f.height        = m_height;
        f.stride        = size_t(m_width) * 4;
        f.data          = m_pixels.data();
        f.fourcc        = 0x34324241; // 'AB24' = DRM_FORMAT_ABGR8888 (R first in mem)
        f.sessionHandle = reinterpret_cast<void*>(uintptr_t(i + 1));
        onFrame(f);
    }
}

void FakeWaylandCaptureSession::releaseBuffer(void* /*sessionHandle*/) {
    m_framesReleased.fetch_add(1, std::memory_order_release);
}

void FakeWaylandCaptureSession::stop() { m_stopped.store(true); }

void FakeWaylandCaptureSession::waitForCompletion() {
    while (m_framesReleased.load(std::memory_order_acquire) < m_framesToEmit) {
        // Tight spin is fine - test fixtures are not perf-sensitive.
    }
}
