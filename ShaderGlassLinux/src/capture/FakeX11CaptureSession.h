#pragma once
#include "X11CaptureSession.h"
#include <vector>
#include <cstdint>
#include <stdexcept>

// Synthetic frame producer for headless tests.
// Stores a fixed BGRA buffer and returns it on every grab() once start() is
// called. Mirrors FakeWaylandCaptureSession's role for M2.
class FakeX11CaptureSession : public X11CaptureSession {
public:
    // `pixels` must be width*height*4 bytes, BGRA layout (matching what
    //  XShmGetImage typically produces on a TrueColor visual).
    FakeX11CaptureSession(uint32_t width, uint32_t height,
                          std::vector<uint8_t> pixels);
    ~FakeX11CaptureSession() override;

    std::vector<SourceInfo>        enumerateSources() override;
    void                           start(const SourceInfo& source) override;
    void                           stop() override;
    std::optional<X11SessionFrame> grab() override;

private:
    uint32_t              m_width, m_height;
    std::vector<uint8_t>  m_pixels;
    bool                  m_started = false;
};
