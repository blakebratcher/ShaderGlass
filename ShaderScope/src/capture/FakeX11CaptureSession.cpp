#include "FakeX11CaptureSession.h"

FakeX11CaptureSession::FakeX11CaptureSession(uint32_t width, uint32_t height,
                                             std::vector<uint8_t> pixels)
    : m_width(width), m_height(height), m_pixels(std::move(pixels)) {
    if (m_pixels.size() != size_t(width) * height * 4) {
        throw std::invalid_argument("FakeX11CaptureSession: pixels size mismatch");
    }
}

FakeX11CaptureSession::~FakeX11CaptureSession() = default;

std::vector<SourceInfo> FakeX11CaptureSession::enumerateSources() {
    return {
        { "monitor:root", "Monitor: full root (fake)" },
        { "window:0xfake", "Window: Fake Window (fake-class)" },
    };
}

void FakeX11CaptureSession::start(const SourceInfo& /*source*/) {
    m_started = true;
}

void FakeX11CaptureSession::stop() {
    m_started = false;
}

std::optional<X11SessionFrame> FakeX11CaptureSession::grab() {
    if (!m_started) return std::nullopt;
    X11SessionFrame f;
    f.data   = m_pixels.data();
    f.stride = size_t(m_width) * 4;
    f.fourcc = 0x34325241;  // DRM_FORMAT_ARGB8888 — BGRA in memory
    f.width  = m_width;
    f.height = m_height;
    return f;
}
