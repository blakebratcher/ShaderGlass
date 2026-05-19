#include "WaylandCapture.h"

WaylandCapture::WaylandCapture(std::unique_ptr<WaylandCaptureSession> session)
    : m_session(std::move(session)) {}

WaylandCapture::~WaylandCapture() {
    if (m_session && m_started.load()) m_session->stop();
}

std::vector<SourceInfo> WaylandCapture::enumerateSources() {
    if (m_sources.empty()) m_sources = m_session->selectSource();
    return m_sources;
}

void WaylandCapture::selectSource(const SourceInfo& /*src*/) {
    // The portal handed us a single source via selectSource(); for M2 we
    // honor whichever the user picked, regardless of the SourceInfo passed
    // in (the public CaptureBackend API doesn't have a way to pass through
    // user-mediated selection). Future iterations may expose this.
    if (!m_started.exchange(true)) {
        m_session->start([this](const CapturedFrame& f) { onFrame(f); });
    }
}

std::optional<CapturedFrame> WaylandCapture::acquireFrame() {
    std::lock_guard<std::mutex> g(m_slotMutex);
    auto out = m_latest;
    m_latest.reset();
    return out;
}

void WaylandCapture::release(CapturedFrame& f) {
    if (f.sessionHandle) m_session->releaseBuffer(f.sessionHandle);
    f.sessionHandle = nullptr;
}

void WaylandCapture::onFrame(const CapturedFrame& f) {
    void* stale = nullptr;
    {
        std::lock_guard<std::mutex> g(m_slotMutex);
        if (m_latest && m_latest->sessionHandle) {
            stale = m_latest->sessionHandle;
        }
        m_latest = f;
    }
    m_lastWidth.store((int)f.width);
    m_lastHeight.store((int)f.height);
    // Release the stale frame outside the slot mutex so a future blocking
    // releaseBuffer (or one that re-enters onFrame) doesn't deadlock.
    if (stale) m_session->releaseBuffer(stale);
}

CaptureBackend::Size WaylandCapture::size() const {
    return { m_lastWidth.load(), m_lastHeight.load() };
}

std::string WaylandCapture::consumeLastError() {
    return m_session ? m_session->consumeLastError() : std::string{};
}
