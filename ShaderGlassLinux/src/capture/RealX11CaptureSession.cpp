#include "RealX11CaptureSession.h"
#include "util/Logging.h"
#include <X11/extensions/Xcomposite.h>
#include <stdexcept>

RealX11CaptureSession::RealX11CaptureSession() {
    m_display = XOpenDisplay(nullptr);
    if (!m_display) {
        throw std::runtime_error("X11: cannot open DISPLAY (is X server running?)");
    }
    m_root = DefaultRootWindow(m_display);

    int evb, erb;
    if (!XCompositeQueryExtension(m_display, &evb, &erb)) {
        XCloseDisplay(m_display);
        m_display = nullptr;
        throw std::runtime_error("X11: server missing Composite extension");
    }

    m_haveXShm = (XShmQueryExtension(m_display) == True);
    if (!m_haveXShm) {
        LOG_WARN("XShm unavailable, will fall back to slow XGetImage path");
    }
}

RealX11CaptureSession::~RealX11CaptureSession() {
    stop();
    if (m_display) {
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

// --- placeholders implemented in subsequent tasks ---

std::vector<SourceInfo> RealX11CaptureSession::enumerateSources() {
    return {};   // Task 8 + Task 11 fill this in.
}

void RealX11CaptureSession::start(const SourceInfo& /*source*/) {
    throw std::runtime_error("RealX11CaptureSession::start not yet implemented");
}

void RealX11CaptureSession::stop() {
    // No-op until Task 9 fills it in. Safe to call before resources are
    // allocated.
}

std::optional<X11SessionFrame> RealX11CaptureSession::grab() {
    return std::nullopt;  // Task 10 fills this in.
}

void RealX11CaptureSession::allocSharedImage(uint32_t /*w*/, uint32_t /*h*/) {}
void RealX11CaptureSession::freeSharedImage() {}
bool RealX11CaptureSession::reallocIfDimsChanged(uint32_t, uint32_t) { return true; }
Drawable RealX11CaptureSession::targetDrawable() const { return m_root; }
