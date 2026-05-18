#include "X11Capture.h"

X11Capture::X11Capture(std::unique_ptr<X11CaptureSession> session)
    : m_session(std::move(session)) {}

X11Capture::~X11Capture() {
    if (m_session && m_started.load()) m_session->stop();
}

std::vector<SourceInfo> X11Capture::enumerateSources() {
    if (m_sources.empty()) m_sources = m_session->enumerateSources();
    return m_sources;
}

void X11Capture::selectSource(const SourceInfo& src) {
    // Unlike M2 (where the portal pre-selects), X11 honors the caller's pick.
    if (!m_started.exchange(true)) {
        m_session->start(src);
    }
}

std::optional<CapturedFrame> X11Capture::acquireFrame() {
    std::lock_guard<std::mutex> g(m_grabMutex);
    if (!m_started.load()) return std::nullopt;
    auto raw = m_session->grab();
    if (!raw) return std::nullopt;

    CapturedFrame f;
    f.kind          = CapturedFrame::Kind::CpuBuffer;
    f.width         = raw->width;
    f.height        = raw->height;
    f.stride        = raw->stride;
    f.fourcc        = raw->fourcc;
    f.data          = raw->data;
    f.sessionHandle = nullptr;  // no per-frame resource for X11/SHM
    return f;
}

void X11Capture::release(CapturedFrame& f) {
    // No-op: the SHM segment persists across grabs and is freed by stop().
    f.data          = nullptr;
    f.sessionHandle = nullptr;
}
