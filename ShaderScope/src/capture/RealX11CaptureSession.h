#pragma once
#include "X11CaptureSession.h"
#include "../render/DmaBufImport.h"
#include <X11/X.h>     // Atom
#include <X11/Xlib.h>
#include <X11/extensions/XShm.h>
#include <sys/shm.h>
#include <cstdint>
#include <optional>
#include <vector>
#include <string>

class VulkanContext;

class RealX11CaptureSession : public X11CaptureSession {
public:
    RealX11CaptureSession();
    ~RealX11CaptureSession() override;

    RealX11CaptureSession(const RealX11CaptureSession&)            = delete;
    RealX11CaptureSession& operator=(const RealX11CaptureSession&) = delete;

    // Wire a VulkanContext to enable the DRI3 DMA-BUF zero-copy fast path.
    // Must be called before start(). If left null, grab() always uses the
    // IncludeInferiors XCopyArea + XShmGetImage CPU path.
    void setVulkanContext(VulkanContext* ctx) { m_vkCtx = ctx; }

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
    bool        m_windowUnmapped = false; // XWindow: tracked via Map/UnmapNotify

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

    // Composited-capture staging (Part 1). For monitor sources we XCopyArea
    // the root (IncludeInferiors) into this server-side pixmap so the grab
    // reflects the composed screen even under GLX-backend compositors. The GC
    // carries subwindow_mode = IncludeInferiors.
    Pixmap         m_stagingPixmap = 0;
    GC             m_copyGC        = nullptr;
    bool           m_haveStaging   = false;

    // DRI3 DMA-BUF fast path (Part 2). Null context → CPU path only.
    VulkanContext* m_vkCtx          = nullptr;
    bool           m_dmaBufActive   = false;  // decided once in start()
    bool           m_dmaImportValid = false;  // m_dmaImport holds a live import
    ImportedDmaBuf m_dmaImport{};
    int            m_dmaImportW     = 0;       // dims the cached import was built for
    int            m_dmaImportH     = 0;
    uint64_t       m_dmaImportPixmap = 0;      // pixmap XID the cached import was built for

    // XRandR event base (for RRScreenChangeNotify dispatch). -1 = no XRandR.
    int            m_xrandrEventBase = -1;

    // Cached EWMH/ICCCM atoms — interned once at construction so enumerateSources
    // doesn't pay a server roundtrip per window per call.
    Atom m_atomNetWmState       = 0;
    Atom m_atomNetWmStateHidden = 0;
    Atom m_atomNetWmName        = 0;
    Atom m_atomUtf8String       = 0;

    // Internals.
    void   allocSharedImage(uint32_t w, uint32_t h);
    void   freeSharedImage();
    bool   reallocIfDimsChanged(uint32_t newW, uint32_t newH);
    Drawable targetDrawable() const;
    bool   windowHasHiddenState(Window w) const;

    // Bug 1 (XWindow): drain pending StructureNotify events for m_windowTarget.
    // On any Map/Unmap/ConfigureNotify the composite backing pixmap was
    // reallocated, so re-name it and invalidate the staging + DMA caches.
    // Returns true if the pixmap was re-named (caller should skip the frame if
    // the window is now unmapped).
    void   drainWindowStructureEvents();
    bool   renameWindowPixmap();

    // Bug 2 (MonitorOutput): re-query the CRTC crop for m_outputName (same
    // logic as start()). Returns false if the output is gone. On success it
    // writes the new crop/size into the out-params (caller decides whether
    // anything changed).
    bool   queryOutputCrop(int& cropX, int& cropY, uint32_t& w, uint32_t& h) const;

    // Composited-capture helpers (Part 1).
    bool   ensureStaging(uint32_t w, uint32_t h);  // (re)create pixmap + GC
    void   freeStaging();
    // True for sources that read from the root and therefore need the
    // IncludeInferiors composite copy (MonitorRoot / MonitorOutput). Window
    // sources read their own redirected backing pixmap and skip the copy.
    bool   needsCompositeCopy() const;

    // DRI3 DMA-BUF helpers (Part 2).
    bool   detectDmaBufPath();                     // env + DRI3 + Vulkan checks
    bool   ensureDmaImport(Drawable pixmap, uint32_t w, uint32_t h);
    void   freeDmaImport();
    std::optional<X11SessionFrame> grabViaDmaBuf();
};
