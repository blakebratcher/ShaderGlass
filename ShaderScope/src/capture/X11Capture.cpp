#include "X11Capture.h"
#include "RealX11CaptureSession.h"

X11Capture::X11Capture(std::unique_ptr<X11CaptureSession> session)
    : m_session(std::move(session)) {}

void X11Capture::setVulkanContext(VulkanContext* ctx) {
    // Only the real session knows about Vulkan/DRI3; the fake one ignores it.
    if (auto* real = dynamic_cast<RealX11CaptureSession*>(m_session.get())) {
        real->setVulkanContext(ctx);
    }
}

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
    f.width         = raw->width;
    f.height        = raw->height;
    f.fourcc        = raw->fourcc;
    f.sessionHandle = nullptr;  // no per-frame resource for X11/SHM
    if (raw->importedDmaBuf) {
        // DRI3 zero-copy fast path: the session owns a cached ImportedDmaBuf
        // kept current via its per-frame XCopyArea. No CPU pixels to ship.
        f.kind           = CapturedFrame::Kind::DmaBuf;
        f.modifier       = raw->modifier;
        f.importedDmaBuf = raw->importedDmaBuf;
    } else {
        f.kind   = CapturedFrame::Kind::CpuBuffer;
        f.stride = raw->stride;
        f.data   = raw->data;
    }
    m_lastWidth.store((int)raw->width);
    m_lastHeight.store((int)raw->height);
    return f;
}

CaptureBackend::Size X11Capture::size() const {
    int w = m_lastWidth.load(), h = m_lastHeight.load();
    if (w == 0 && h == 0 && m_session) {
        auto [sw, sh] = m_session->size();
        return { sw, sh };
    }
    return { w, h };
}

void X11Capture::release(CapturedFrame& f) {
    // No-op: the SHM segment persists across grabs and is freed by stop().
    f.data          = nullptr;
    f.sessionHandle = nullptr;
}
