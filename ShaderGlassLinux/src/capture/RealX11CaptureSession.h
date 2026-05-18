#pragma once
#include "X11CaptureSession.h"
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

class RealX11CaptureSession : public X11CaptureSession {
public:
    RealX11CaptureSession();
    ~RealX11CaptureSession() override;

    RealX11CaptureSession(const RealX11CaptureSession&)            = delete;
    RealX11CaptureSession& operator=(const RealX11CaptureSession&) = delete;

    std::vector<SourceInfo>           enumerateSources() override;
    void                              start(const SourceInfo& source) override;
    void                              stop() override;
    std::optional<X11SessionFrame>    grab() override;

private:
    // Source identification (parsed from SourceInfo::id at start()).
    // Note: avoid Xlib macro names (None=0, Window=typedef) as enumerators.
    enum class SourceKind { Unset, MonitorRoot, MonitorOutput, XWindow };
    SourceKind  m_sourceKind = SourceKind::Unset;
    std::string m_outputName;          // for MonitorOutput
    Window      m_windowTarget = 0;    // for XWindow source kind (Xlib Window type)
    int         m_cropX = 0, m_cropY = 0; // for MonitorOutput
    uint32_t    m_width = 0, m_height = 0;

    // X resources.
    Display*       m_display      = nullptr;
    Window         m_root         = 0;
    bool           m_haveXShm     = false;
    bool           m_havePixmap   = false;
    Pixmap         m_pixmap       = 0;  // window backing pixmap
    XImage*        m_image        = nullptr;
    XShmSegmentInfo m_shm{};
    bool           m_shmAttached  = false;
    std::vector<uint8_t> m_xgetImageStaging; // staging buffer for XGetImage fallback path

    // Internals.
    void   allocSharedImage(uint32_t w, uint32_t h);
    void   freeSharedImage();
    bool   reallocIfDimsChanged(uint32_t newW, uint32_t newH);
    Drawable targetDrawable() const;
};
