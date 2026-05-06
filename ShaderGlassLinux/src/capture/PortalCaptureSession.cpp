#include "PortalCaptureSession.h"
#include "../util/Logging.h"
#include <stdexcept>

PortalCaptureSession::PortalCaptureSession() = default;
PortalCaptureSession::~PortalCaptureSession() = default;

std::vector<SourceInfo> PortalCaptureSession::selectSource() {
    doPortalHandshake();   // Task 6
    return { { "wayland-screen://" + std::to_string(m_pipewireNodeId),
               "wayland-screen (node " + std::to_string(m_pipewireNodeId) + ")" } };
}

void PortalCaptureSession::start(std::function<void(const CapturedFrame&)> onFrame) {
    m_onFrame = std::move(onFrame);
    initPipeWire();        // Task 8
    m_running.store(true);
}

void PortalCaptureSession::releaseBuffer(void* /*sessionHandle*/) {
    // Task 8 fills this in.
}

void PortalCaptureSession::stop() {
    if (m_running.exchange(false)) {
        teardownPipeWire();
    }
}

void PortalCaptureSession::doPortalHandshake() {
    throw std::runtime_error("PortalCaptureSession::doPortalHandshake — not yet implemented (Task 6)");
}

void PortalCaptureSession::initPipeWire() {
    throw std::runtime_error("PortalCaptureSession::initPipeWire — not yet implemented (Task 8)");
}

void PortalCaptureSession::teardownPipeWire() {
    // Stub for Task 8.
}

void PortalCaptureSession::onProcessThunk(void* /*userdata*/) {}
void PortalCaptureSession::onParamChangedThunk(void* /*userdata*/, uint32_t /*id*/, const struct spa_pod* /*param*/) {}
void PortalCaptureSession::onProcess() {}
void PortalCaptureSession::onParamChanged(uint32_t /*id*/, const struct spa_pod* /*param*/) {}
