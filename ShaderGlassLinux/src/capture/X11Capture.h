#pragma once
#include "CaptureBackend.h"
#include "X11CaptureSession.h"
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class X11Capture : public CaptureBackend {
public:
    explicit X11Capture(std::unique_ptr<X11CaptureSession> session);
    ~X11Capture() override;

    X11Capture(const X11Capture&)            = delete;
    X11Capture& operator=(const X11Capture&) = delete;

    std::vector<SourceInfo>      enumerateSources() override;
    void                         selectSource(const SourceInfo& src) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void                         release(CapturedFrame& f) override;

private:
    std::unique_ptr<X11CaptureSession> m_session;
    std::vector<SourceInfo>            m_sources;
    std::atomic<bool>                  m_started{false};
    std::mutex                         m_grabMutex;  // serializes grab() calls
};
