#pragma once
#include "CaptureBackend.h"
#include "WaylandCaptureSession.h"
#include <memory>
#include <mutex>
#include <optional>
#include <atomic>

class WaylandCapture : public CaptureBackend {
public:
    explicit WaylandCapture(std::unique_ptr<WaylandCaptureSession> session);
    ~WaylandCapture() override;

    WaylandCapture(const WaylandCapture&)            = delete;
    WaylandCapture& operator=(const WaylandCapture&) = delete;

    std::string                  kindName() const override { return "wayland-screen"; }
    std::vector<SourceInfo>      enumerateSources() override;
    void                         selectSource(const SourceInfo&) override;
    std::optional<CapturedFrame> acquireFrame() override;
    void                         release(CapturedFrame&) override;

private:
    void onFrame(const CapturedFrame& f);

    std::unique_ptr<WaylandCaptureSession> m_session;
    std::mutex                             m_slotMutex;
    std::optional<CapturedFrame>           m_latest;
    std::atomic<bool>                      m_started{false};
    std::vector<SourceInfo>                m_sources;
};
