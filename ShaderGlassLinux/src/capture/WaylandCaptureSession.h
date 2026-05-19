#pragma once
#include "CapturedFrame.h"
#include "CaptureBackend.h"  // for SourceInfo
#include <functional>
#include <vector>
#include <stdexcept>

// Boundary between WaylandCapture (CaptureBackend impl) and the actual
// pixel source. Production impl: PortalCaptureSession (D-Bus + PipeWire).
// Test impl:       FakeWaylandCaptureSession (synthetic frames).
//
// Threading: onFrame() may be called from a producer-owned thread (e.g.,
// pw_thread_loop in the portal impl). The callback should not block.
class WaylandCaptureSession {
public:
    virtual ~WaylandCaptureSession() = default;

    // Walks the portal handshake (real impl) or chooses a synthetic source
    // (fake impl). Throws on user-cancelled portal or other unrecoverable
    // setup failures.
    virtual std::vector<SourceInfo> selectSource() = 0;

    // Begins delivering frames. `onFrame` is invoked once per frame; the
    // CapturedFrame's sessionHandle identifies the buffer for later release.
    // Throws on stream-setup failure (e.g., format negotiation rejected).
    virtual void start(std::function<void(const CapturedFrame&)> onFrame) = 0;

    // Releases a buffer back to the source. Safe to call from any thread.
    virtual void releaseBuffer(void* sessionHandle) = 0;

    // Cleanly tear down stream + portal session.
    virtual void stop() = 0;

    // Last error/warning produced by the session that should be surfaced to
    // the user (e.g., unsupported negotiated pixel format). Drained on each
    // call; empty string means nothing to surface. Thread-safe: implementations
    // must protect m_lastError with their own mutex or atomic if written from a
    // PipeWire callback thread.
    virtual std::string consumeLastError() { return {}; }
};

class WaylandCaptureUnsupportedFormat : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};
