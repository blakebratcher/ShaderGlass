#include "RealX11CaptureSession.h"
#include "BadWindowRegistry.h"
#include "X11DmaBufPolicy.h"
#include "X11CaptureGeometry.h"
#include "util/Logging.h"
#include "render/DmaBufImport.h"
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xatom.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if SHADERSCOPE_HAVE_XCB_DRI3
#include <X11/Xlib-xcb.h>
#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <unistd.h>
#endif

namespace {

int x11ErrorHandler(Display* d, XErrorEvent* ev) {
    if (ev->error_code == BadWindow) {
        // Dispatch to the right session by its Display* — important once M4
        // ever runs two RealX11CaptureSession instances concurrently.
        BadWindowRegistry::note(d);
    }
    // Returning 0 tells Xlib not to abort the process.
    return 0;
}

struct X11ErrorHandlerInstaller {
    X11ErrorHandlerInstaller() { XSetErrorHandler(x11ErrorHandler); }
};
X11ErrorHandlerInstaller g_x11ErrorHandlerInstaller;

std::string getStringProp(Display* d, Window w, Atom prop, Atom type) {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;
    std::string out;
    if (XGetWindowProperty(d, w, prop, 0, 1024, False, type,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) == Success && data) {
        out.assign(reinterpret_cast<const char*>(data), nitems);
        XFree(data);
    }
    return out;
}

} // namespace

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

    // Intern EWMH/ICCCM atoms once. enumerateSources() touches every top-level
    // window, so re-interning per call was an N×4 server roundtrip storm.
    m_atomNetWmState       = XInternAtom(m_display, "_NET_WM_STATE",        False);
    m_atomNetWmStateHidden = XInternAtom(m_display, "_NET_WM_STATE_HIDDEN", False);
    m_atomNetWmName        = XInternAtom(m_display, "_NET_WM_NAME",         False);
    m_atomUtf8String       = XInternAtom(m_display, "UTF8_STRING",          False);

    BadWindowRegistry::add(m_display);
}

RealX11CaptureSession::~RealX11CaptureSession() {
    stop();
    if (m_display) {
        BadWindowRegistry::remove(m_display);
        XCloseDisplay(m_display);
        m_display = nullptr;
    }
}

std::vector<SourceInfo> RealX11CaptureSession::enumerateSources() {
    std::vector<SourceInfo> out;

    int rootW = DisplayWidth(m_display, DefaultScreen(m_display));
    int rootH = DisplayHeight(m_display, DefaultScreen(m_display));

    {
        SourceInfo s;
        s.id = "monitor:root";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Monitor: full root (%dx%d)", rootW, rootH);
        s.displayName = buf;
        out.push_back(std::move(s));
    }

    XRRScreenResources* res = XRRGetScreenResources(m_display, m_root);
    if (res) {
        for (int i = 0; i < res->noutput; ++i) {
            XRROutputInfo* oi = XRRGetOutputInfo(m_display, res, res->outputs[i]);
            if (!oi) continue;
            if (oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(m_display, res, oi->crtc);
                if (ci) {
                    SourceInfo s;
                    s.id = std::string("monitor:") + oi->name;
                    char buf[128];
                    std::snprintf(buf, sizeof(buf),
                                  "Monitor: %s (%ux%u)",
                                  oi->name, ci->width, ci->height);
                    s.displayName = buf;
                    out.push_back(std::move(s));
                    XRRFreeCrtcInfo(ci);
                }
            }
            XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
    }

    Window dummyRoot, dummyParent;
    Window* children = nullptr;
    unsigned int nChildren = 0;
    if (XQueryTree(m_display, m_root, &dummyRoot, &dummyParent,
                   &children, &nChildren)) {
        for (unsigned int i = 0; i < nChildren; ++i) {
            Window w = children[i];

            XWindowAttributes attr{};
            if (!XGetWindowAttributes(m_display, w, &attr)) continue;
            if (attr.override_redirect) continue;
            if (attr.map_state != IsViewable && !windowHasHiddenState(w)) {
                // unmapped and not "hidden" (minimized) — skip
                continue;
            }

            std::string name = getStringProp(m_display, w,
                                             m_atomNetWmName, m_atomUtf8String);
            if (name.empty()) continue;  // unnamed: skip

            std::string wmClass = getStringProp(m_display, w,
                                                XA_WM_CLASS, XA_STRING);
            // WM_CLASS is "<instance>\0<class>\0" — keep only the first token.
            if (auto nul = wmClass.find('\0'); nul != std::string::npos) {
                wmClass.resize(nul);
            }

            SourceInfo s;
            char idbuf[32];
            std::snprintf(idbuf, sizeof(idbuf), "window:0x%lx", (unsigned long)w);
            s.id = idbuf;

            char dnbuf[256];
            if (!wmClass.empty()) {
                std::snprintf(dnbuf, sizeof(dnbuf), "Window: %s (%s)",
                              name.c_str(), wmClass.c_str());
            } else {
                std::snprintf(dnbuf, sizeof(dnbuf), "Window: %s", name.c_str());
            }
            s.displayName = dnbuf;
            out.push_back(std::move(s));
        }
        if (children) XFree(children);
    }
    return out;
}

void RealX11CaptureSession::start(const SourceInfo& source) {
    // Parse the SourceInfo::id into a kind + extra info.
    const std::string& id = source.id;
    if (id == "monitor:root") {
        m_sourceKind = SourceKind::MonitorRoot;
        m_width  = DisplayWidth (m_display, DefaultScreen(m_display));
        m_height = DisplayHeight(m_display, DefaultScreen(m_display));
        m_cropX = m_cropY = 0;
    } else if (id.rfind("monitor:", 0) == 0) {
        m_sourceKind  = SourceKind::MonitorOutput;
        m_outputName  = id.substr(strlen("monitor:"));
        // Look up the output's CRTC for crop info + dimensions.
        if (!queryOutputCrop(m_cropX, m_cropY, m_width, m_height)) {
            throw std::runtime_error("X11: output not found: " + m_outputName);
        }
        // Bug 2: watch for XRandR mode/layout changes so grab() can re-query
        // the crop and drop frames where the source rect no longer fits.
        int xrErr = 0;
        if (XRRQueryExtension(m_display, &m_xrandrEventBase, &xrErr)) {
            XRRSelectInput(m_display, m_root, RRScreenChangeNotifyMask);
        } else {
            m_xrandrEventBase = -1;
        }
    } else if (id.rfind("window:", 0) == 0) {
        m_sourceKind = SourceKind::XWindow;
        unsigned long xid = 0;
        if (std::sscanf(id.c_str() + strlen("window:"), "0x%lx", &xid) != 1) {
            throw std::runtime_error("X11: bad window id: " + id);
        }
        m_windowTarget = (Window)xid;
        m_windowUnmapped = false;

        // Composite-redirect so we can capture even when the window is
        // partially obscured or minimized. The pixmap is the off-screen
        // backing store the composite manager renders into.
        XCompositeRedirectWindow(m_display, m_windowTarget,
                                 CompositeRedirectAutomatic);

        // Bug 1: the composite backing pixmap is reallocated on EVERY
        // map/unmap/reconfigure (Composite spec), not just on size change.
        // Watch StructureNotify so grab() can re-name the pixmap and
        // invalidate the staging + DMA caches when that happens. Selecting
        // input before XCompositeNameWindowPixmap means we won't miss an
        // event that races the initial name.
        XSelectInput(m_display, m_windowTarget, StructureNotifyMask);
        XSync(m_display, False);

        m_pixmap = XCompositeNameWindowPixmap(m_display, m_windowTarget);
        if (!m_pixmap) {
            XCompositeUnredirectWindow(m_display, m_windowTarget,
                                       CompositeRedirectAutomatic);
            throw std::runtime_error("X11: XCompositeNameWindowPixmap failed");
        }
        m_havePixmap = true;

        XWindowAttributes attr{};
        if (!XGetWindowAttributes(m_display, m_windowTarget, &attr)) {
            throw std::runtime_error("X11: XGetWindowAttributes failed");
        }
        m_width  = attr.width;
        m_height = attr.height;
        m_cropX  = m_cropY = 0;
    } else {
        throw std::runtime_error("X11: unrecognized source id: " + id);
    }

    // Decide the DMA-BUF fast path once per session (env + DRI3 + Vulkan).
    m_dmaBufActive = detectDmaBufPath();
    if (m_dmaBufActive) {
        LOG_INFO("X11: DRI3 DMA-BUF fast path enabled");
    } else {
        LOG_INFO("X11: using CPU capture path (IncludeInferiors XCopyArea + XShm)");
    }

    allocSharedImage(m_width, m_height);
}

void RealX11CaptureSession::stop() {
    freeDmaImport();
    freeStaging();
    freeSharedImage();
    if (m_havePixmap && m_pixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = 0;
        m_havePixmap = false;
    }
    if (m_sourceKind == SourceKind::XWindow && m_windowTarget) {
        // Best-effort. If the window's already gone this is a no-op
        // (and the error handler installed in Task 15 swallows the BadWindow).
        XCompositeUnredirectWindow(m_display, m_windowTarget,
                                   CompositeRedirectAutomatic);
        m_windowTarget = 0;
    }
    m_dmaBufActive = false;
    m_sourceKind = SourceKind::Unset;
    m_width = m_height = 0;
    m_windowUnmapped = false;
    m_xrandrEventBase = -1;
}

std::optional<X11SessionFrame> RealX11CaptureSession::grab() {
    if (m_sourceKind == SourceKind::Unset) return std::nullopt;

    // Bug 1 (XWindow): drain StructureNotify events BEFORE touching the
    // pixmap. A map/unmap/reconfigure reallocates the composite backing
    // pixmap (Composite spec), so we re-name it here and invalidate the
    // staging + DMA caches; otherwise every grab returns frozen content.
    if (m_sourceKind == SourceKind::XWindow) {
        drainWindowStructureEvents();
        if (m_windowUnmapped) {
            // Nothing to capture while the window is hidden/minimized; its
            // backing pixmap is undefined. Skip this frame.
            return std::nullopt;
        }
    }

    // Bug 2 (MonitorOutput): drain XRandR screen-change events and re-query
    // the CRTC crop when the mode/layout changed. Done before the read so a
    // stale crop never feeds XCopyArea (which would silently keep old pixels
    // in any region that fell out of bounds).
    if (m_sourceKind == SourceKind::MonitorOutput && m_xrandrEventBase >= 0) {
        bool screenChanged = false;
        XEvent ev;
        while (XCheckTypedEvent(
                   m_display, m_xrandrEventBase + RRScreenChangeNotify, &ev)) {
            // Let Xlib update its cached screen config from the event.
            XRRUpdateConfiguration(&ev);
            screenChanged = true;
        }
        if (screenChanged) {
            int newX, newY; uint32_t newW, newH;
            if (!queryOutputCrop(newX, newY, newW, newH)) {
                LOG_ERROR("X11: monitor output '%s' gone after XRandR change",
                          m_outputName.c_str());
                m_sourceKind = SourceKind::Unset;
                return std::nullopt;
            }
            if (newX != m_cropX || newY != m_cropY ||
                newW != m_width || newH != m_height) {
                freeDmaImport();
                freeStaging();
                freeSharedImage();
                m_cropX = newX; m_cropY = newY;
                m_width = newW; m_height = newH;
                allocSharedImage(m_width, m_height);
                return std::nullopt;  // skip the frame; resources are fresh
            }
        }
    }

    // Detect source resize. Cheap query (no server roundtrip cache).
    Window  rootRet;
    int     xRet, yRet;
    unsigned int wRet, hRet, borderRet, depthRet;
    Drawable probe = (m_sourceKind == SourceKind::XWindow)
                     ? (Drawable)m_windowTarget
                     : (Drawable)m_root;
    if (XGetGeometry(m_display, probe, &rootRet, &xRet, &yRet,
                     &wRet, &hRet, &borderRet, &depthRet)) {
        if (m_sourceKind == SourceKind::MonitorOutput) {
            // Bug 2 belt-and-braces: even if we missed the XRandR event,
            // validate the cached crop against the live root size. If the
            // source rect no longer fits, re-query (or drop the frame) rather
            // than reading partially out of bounds.
            if (!X11CaptureGeometry::cropFitsRoot(m_cropX, m_cropY,
                                                  m_width, m_height,
                                                  wRet, hRet)) {
                int newX, newY; uint32_t newW, newH;
                if (!queryOutputCrop(newX, newY, newW, newH)) {
                    LOG_ERROR("X11: monitor output '%s' gone (crop out of root)",
                              m_outputName.c_str());
                    m_sourceKind = SourceKind::Unset;
                    return std::nullopt;
                }
                freeDmaImport();
                freeStaging();
                freeSharedImage();
                m_cropX = newX; m_cropY = newY;
                m_width = newW; m_height = newH;
                allocSharedImage(m_width, m_height);
                return std::nullopt;  // skip the frame; resources are fresh
            }
        } else if (reallocIfDimsChanged(wRet, hRet)) {
            // Skip this frame — the new SHM segment is fresh and uninitialised.
            return std::nullopt;
        }
    }

    if (m_sourceKind == SourceKind::XWindow &&
        BadWindowRegistry::consume(m_display)) {
        LOG_ERROR("source window 0x%lx was destroyed",
                  (unsigned long)m_windowTarget);
        m_havePixmap = false;
        m_windowTarget = 0;
        m_sourceKind = SourceKind::Unset;
        return std::nullopt;
    }

    // Composited capture (Part 1): for monitor sources, server-side-copy the
    // root window — with subwindow_mode = IncludeInferiors — into a staging
    // pixmap. The copy includes the composite overlay window's content, so it
    // reflects the composed screen even under GLX-backend compositors (picom
    // backend="glx" etc.), where the root pixmap itself only holds wallpaper.
    //
    // After a successful copy, the staging pixmap holds the cropped region at
    // origin (0,0); both the DMA-BUF and CPU read paths below target it.
    Drawable readDrawable = targetDrawable();
    if (readDrawable == 0) return std::nullopt;
    int readX = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropX : 0;
    int readY = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropY : 0;

    if (needsCompositeCopy() && ensureStaging(m_width, m_height)) {
        XCopyArea(m_display, m_root, m_stagingPixmap, m_copyGC,
                  readX, readY, m_width, m_height, 0, 0);
        readDrawable = m_stagingPixmap;
        readX = readY = 0;
    }

    // DMA-BUF fast path (Part 2): export the staging (or window backing)
    // pixmap via DRI3 and hand the consumer a zero-copy VkImage. The XCopyArea
    // above already refreshed the pixmap's GPU contents this frame.
    if (m_dmaBufActive) {
        if (auto dmaFrame = grabViaDmaBuf()) {
            return dmaFrame;
        }
        // Import/export failed — disable for the rest of the session and fall
        // through to the CPU path, which always works.
        LOG_WARN("X11: DRI3 DMA-BUF export failed; falling back to CPU path");
        freeDmaImport();
        m_dmaBufActive = false;
    }

    if (!m_haveXShm) {
        // XGetImage allocates a new XImage each call; we copy out the data
        // into a thread-local staging buffer so the caller's pointer
        // lifetime matches the rest of the contract.
        XImage* img = XGetImage(m_display, readDrawable, readX, readY,
                                m_width, m_height, AllPlanes, ZPixmap);
        if (!img) return std::nullopt;
        m_xgetImageStaging.assign(img->data, img->data + size_t(img->bytes_per_line) * img->height);

        X11SessionFrame f;
        f.data   = m_xgetImageStaging.data();
        f.stride = img->bytes_per_line;
        f.fourcc = 0x34325241;  // ARGB8888 (BGRA in memory) on TrueColor visuals
        f.width  = m_width;
        f.height = m_height;
        XDestroyImage(img);
        return f;
    }

    Drawable d    = readDrawable;
    int      srcX = readX;
    int      srcY = readY;

    if (!XShmGetImage(m_display, d, m_image, srcX, srcY, AllPlanes)) {
        // Try one teardown + reallocate at current size in case of transient
        // X server hiccup. Resize handling is layered on in Task 13.
        freeSharedImage();
        allocSharedImage(m_width, m_height);
        if (!XShmGetImage(m_display, d, m_image, srcX, srcY, AllPlanes)) {
            return std::nullopt;
        }
    }

    X11SessionFrame f;
    f.data   = reinterpret_cast<const uint8_t*>(m_image->data);
    f.stride = m_image->bytes_per_line;
    // On a 32-bit TrueColor visual the in-memory layout is BGRA, which is
    // DRM_FORMAT_ARGB8888 (= 0x34325241). If we ever encounter another
    // visual, fourcc_to_vk() will return VK_FORMAT_UNDEFINED and main.cpp
    // will exit cleanly.
    f.fourcc = 0x34325241;
    f.width  = m_width;
    f.height = m_height;
    return f;
}

void RealX11CaptureSession::allocSharedImage(uint32_t w, uint32_t h) {
    if (!m_haveXShm) {
        // XGetImage path: we allocate no SHM resources. XGetImage will return
        // a fresh XImage* each call (freed by XDestroyImage). We only use
        // m_image as a sentinel here; the slow grab path in grab() does the
        // per-call alloc itself.
        m_image = nullptr;
        m_width  = w;
        m_height = h;
        return;
    }

    int screen = DefaultScreen(m_display);
    Visual* visual = DefaultVisual(m_display, screen);
    int depth = DefaultDepth(m_display, screen);

    m_image = XShmCreateImage(m_display, visual, depth, ZPixmap,
                              nullptr, &m_shm, w, h);
    if (!m_image) throw std::runtime_error("X11: XShmCreateImage failed");

    m_shm.shmid    = shmget(IPC_PRIVATE,
                            size_t(m_image->bytes_per_line) * m_image->height,
                            IPC_CREAT | 0600);
    if (m_shm.shmid == -1) {
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: shmget failed");
    }
    m_shm.shmaddr  = (char*)shmat(m_shm.shmid, nullptr, 0);
    if (m_shm.shmaddr == (char*)-1) {
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: shmat failed");
    }
    // Mark for deletion immediately — the segment stays alive as long as X
    // server attaches via XShmAttach. Standard idiom; closes the leak window
    // between here and XSync.
    shmctl(m_shm.shmid, IPC_RMID, nullptr);
    m_image->data  = m_shm.shmaddr;
    m_shm.readOnly = False;

    if (!XShmAttach(m_display, &m_shm)) {
        shmdt(m_shm.shmaddr);
        // (shmctl IPC_RMID already done above)
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: XShmAttach failed");
    }
    XSync(m_display, False);  // server-side attach must complete
    m_shmAttached = true;
}

void RealX11CaptureSession::freeSharedImage() {
    if (m_shmAttached) {
        XShmDetach(m_display, &m_shm);
        m_shmAttached = false;
    }
    if (m_shm.shmaddr && m_shm.shmaddr != (char*)-1) {
        shmdt(m_shm.shmaddr);
        m_shm.shmaddr = nullptr;
    }
    if (m_image) {
        XDestroyImage(m_image);  // also frees data == shmaddr safely; we
                                 // already detached the segment above.
        m_image = nullptr;
    }
}

bool RealX11CaptureSession::reallocIfDimsChanged(uint32_t newW, uint32_t newH) {
    if (newW == m_width && newH == m_height) return false;
    freeDmaImport();   // stale dims; rebuilt on next grab at the new size
    freeStaging();     // stale dims; rebuilt on next grab at the new size
    freeSharedImage();
    if (m_sourceKind == SourceKind::XWindow && m_havePixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = XCompositeNameWindowPixmap(m_display, m_windowTarget);
        if (!m_pixmap) { m_havePixmap = false; return true; }
    }
    m_width  = newW;
    m_height = newH;
    allocSharedImage(m_width, m_height);
    return true;
}

Drawable RealX11CaptureSession::targetDrawable() const {
    if (m_sourceKind == SourceKind::XWindow) {
        // Window source without a backing pixmap (post-resize failure, etc.)
        // returns 0 to signal "no target"; grab() converts that to nullopt
        // rather than silently capturing the whole desktop.
        return m_havePixmap ? m_pixmap : 0;
    }
    return m_root;
}

// Bug 1: re-name the window's composite backing pixmap. Called after a
// Map/Unmap/Configure event reallocated it server-side. Frees the old name,
// queries a new one, and invalidates the staging + DMA-BUF caches so they
// rebuild against the fresh pixmap. Returns false (and clears m_havePixmap)
// if the new name fails.
bool RealX11CaptureSession::renameWindowPixmap() {
    if (m_sourceKind != SourceKind::XWindow) return false;
    if (m_havePixmap && m_pixmap) {
        XFreePixmap(m_display, m_pixmap);
        m_pixmap = 0;
    }
    m_pixmap = XCompositeNameWindowPixmap(m_display, m_windowTarget);
    // Caches were built against the old pixmap XID; drop them.
    freeDmaImport();
    freeStaging();
    if (!m_pixmap) {
        m_havePixmap = false;
        return false;
    }
    m_havePixmap = true;
    return true;
}

// Bug 1: drain pending StructureNotify events for the source window. Any
// Map/Unmap/Configure means the backing pixmap was reallocated; re-name it.
// Tracks map state in m_windowUnmapped so grab() can skip frames while the
// window is hidden (its backing pixmap is undefined when unmapped).
void RealX11CaptureSession::drainWindowStructureEvents() {
    if (m_sourceKind != SourceKind::XWindow || !m_windowTarget) return;

    bool pixmapStale = false;
    XEvent ev;
    while (XCheckWindowEvent(m_display, m_windowTarget,
                             StructureNotifyMask, &ev)) {
        switch (ev.type) {
            case MapNotify:
                m_windowUnmapped = false;
                pixmapStale = true;
                break;
            case UnmapNotify:
                m_windowUnmapped = true;
                pixmapStale = true;
                break;
            case ConfigureNotify:
                pixmapStale = true;
                break;
            case DestroyNotify:
                // Nothing left to capture. Mark unmapped so we skip the
                // re-name below; the BadWindowRegistry::consume() check in
                // grab() does the full session teardown.
                m_havePixmap = false;
                m_windowUnmapped = true;
                break;
            default:
                break;
        }
    }

    // Only re-name while mapped — XCompositeNameWindowPixmap on an unmapped
    // window yields an unusable pixmap. We rebuild it on the next MapNotify.
    if (pixmapStale && !m_windowUnmapped) {
        if (!renameWindowPixmap()) {
            LOG_WARN("X11: re-name of window 0x%lx backing pixmap failed",
                     (unsigned long)m_windowTarget);
        }
    }
}

// Bug 2: re-query the CRTC crop + size for m_outputName. Shared by start()
// and grab(). Returns false if the output is no longer connected / has no
// CRTC (caller treats that as "source gone").
bool RealX11CaptureSession::queryOutputCrop(int& cropX, int& cropY,
                                            uint32_t& w, uint32_t& h) const {
    XRRScreenResources* res = XRRGetScreenResources(m_display, m_root);
    if (!res) return false;
    bool found = false;
    for (int i = 0; i < res->noutput && !found; ++i) {
        XRROutputInfo* oi = XRRGetOutputInfo(m_display, res, res->outputs[i]);
        if (oi && oi->name && m_outputName == oi->name &&
            oi->connection == RR_Connected && oi->crtc) {
            XRRCrtcInfo* ci = XRRGetCrtcInfo(m_display, res, oi->crtc);
            if (ci) {
                cropX = ci->x;
                cropY = ci->y;
                w     = ci->width;
                h     = ci->height;
                XRRFreeCrtcInfo(ci);
                found = true;
            }
        }
        if (oi) XRRFreeOutputInfo(oi);
    }
    XRRFreeScreenResources(res);
    return found;
}

bool RealX11CaptureSession::windowHasHiddenState(Window w) const {
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;
    bool isHidden = false;
    if (XGetWindowProperty(m_display, w, m_atomNetWmState, 0, 128, False, XA_ATOM,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) == Success && data) {
        const Atom* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == m_atomNetWmStateHidden) { isHidden = true; break; }
        }
        XFree(data);
    }
    return isHidden;
}

// ---------------------------------------------------------------------------
// Composited capture (Part 1): IncludeInferiors XCopyArea staging
// ---------------------------------------------------------------------------

bool RealX11CaptureSession::needsCompositeCopy() const {
    // Window sources read their own composite-redirected backing pixmap, which
    // already holds the window's full content. Only root-reading sources need
    // the IncludeInferiors copy to capture the composed overlay.
    return m_sourceKind == SourceKind::MonitorRoot ||
           m_sourceKind == SourceKind::MonitorOutput;
}

bool RealX11CaptureSession::ensureStaging(uint32_t w, uint32_t h) {
    if (m_haveStaging && m_stagingPixmap && m_copyGC) return true;

    int screen = DefaultScreen(m_display);
    int depth  = DefaultDepth(m_display, screen);

    m_stagingPixmap = XCreatePixmap(m_display, m_root, w, h, depth);
    if (!m_stagingPixmap) {
        LOG_WARN("X11: XCreatePixmap(%ux%u) failed; using direct-root grab", w, h);
        return false;
    }

    XGCValues gcv{};
    gcv.subwindow_mode = IncludeInferiors;
    gcv.graphics_exposures = False;
    m_copyGC = XCreateGC(m_display, m_root,
                         GCSubwindowMode | GCGraphicsExposures, &gcv);
    if (!m_copyGC) {
        LOG_WARN("X11: XCreateGC failed; using direct-root grab");
        XFreePixmap(m_display, m_stagingPixmap);
        m_stagingPixmap = 0;
        return false;
    }

    m_haveStaging = true;
    return true;
}

void RealX11CaptureSession::freeStaging() {
    if (m_copyGC) {
        XFreeGC(m_display, m_copyGC);
        m_copyGC = nullptr;
    }
    if (m_stagingPixmap) {
        XFreePixmap(m_display, m_stagingPixmap);
        m_stagingPixmap = 0;
    }
    m_haveStaging = false;
}

// ---------------------------------------------------------------------------
// DRI3 DMA-BUF fast path (Part 2)
// ---------------------------------------------------------------------------

bool RealX11CaptureSession::detectDmaBufPath() {
    X11DmaBufPolicy::Inputs in;
    in.disabledByEnv = X11DmaBufPolicy::envForcesDisable(
        std::getenv("SHADERSCOPE_DISABLE_X11_DMABUF"));
    in.hasVulkanContext = (m_vkCtx != nullptr);

#if SHADERSCOPE_HAVE_XCB_DRI3
    if (m_vkCtx) {
        in.vulkanSupportsDmaBuf = DmaBufImport::isSupported(*m_vkCtx);
    }
    // Probe DRI3 once. xcb_dri3_query_version returns the server's supported
    // version; any reply (>= 1.0) means buffer(s)_from_pixmap is usable.
    if (in.hasVulkanContext && in.vulkanSupportsDmaBuf && !in.disabledByEnv) {
        xcb_connection_t* conn = XGetXCBConnection(m_display);
        if (conn) {
            auto cookie = xcb_dri3_query_version(conn, 1, 2);
            xcb_generic_error_t* err = nullptr;
            xcb_dri3_query_version_reply_t* reply =
                xcb_dri3_query_version_reply(conn, cookie, &err);
            if (reply) {
                in.dri3Available = true;
                LOG_DEBUG("X11: DRI3 version %u.%u",
                          reply->major_version, reply->minor_version);
                free(reply);
            }
            if (err) free(err);
        }
    }
#endif

    return X11DmaBufPolicy::shouldUseDmaBuf(in);
}

#if SHADERSCOPE_HAVE_XCB_DRI3
bool RealX11CaptureSession::ensureDmaImport(Drawable pixmap, uint32_t w, uint32_t h) {
    // Key on the pixmap XID as well as dims (Bug 1). After a window is
    // minimized + restored at the same size, the backing pixmap XID changes
    // even though w/h don't — a dims-only key would keep the dead import.
    if (X11CaptureGeometry::dmaImportCacheValid(
            m_dmaImportValid, m_dmaImportPixmap, m_dmaImportW, m_dmaImportH,
            (uint64_t)pixmap, (int)w, (int)h)) {
        return true;
    }
    freeDmaImport();

    xcb_connection_t* conn = XGetXCBConnection(m_display);
    if (!conn) return false;

    int      fd        = -1;
    uint32_t stride    = 0;
    uint32_t offset    = 0;
    uint64_t modifier  = 0;  // DRM_FORMAT_MOD_INVALID handled by importer fallback
    bool     haveMulti = false;

    // Prefer the v1.2 multi-plane API (gives us the modifier). Single-plane
    // (ARGB8888) is the only layout we support, so n>1 → bail to CPU.
    auto mcookie = xcb_dri3_buffers_from_pixmap(conn, (xcb_pixmap_t)pixmap);
    xcb_generic_error_t* merr = nullptr;
    xcb_dri3_buffers_from_pixmap_reply_t* mreply =
        xcb_dri3_buffers_from_pixmap_reply(conn, mcookie, &merr);
    if (mreply) {
        if (mreply->nfd >= 1) {
            int* fds = xcb_dri3_buffers_from_pixmap_reply_fds(conn, mreply);
            const uint32_t* strides = xcb_dri3_buffers_from_pixmap_strides(mreply);
            const uint32_t* offsets = xcb_dri3_buffers_from_pixmap_offsets(mreply);
            fd       = fds[0];
            stride   = strides ? strides[0] : w * 4;
            offset   = offsets ? offsets[0] : 0;
            modifier = mreply->modifier;
            haveMulti = true;
            // Close any extra plane fds we won't use (multi-plane unsupported).
            for (int i = 1; i < mreply->nfd; ++i) {
                if (fds[i] >= 0) ::close(fds[i]);
            }
            if (mreply->nfd > 1) {
                LOG_WARN("X11: DRI3 pixmap has %d planes; only single-plane "
                         "ARGB8888 supported — falling back to CPU", mreply->nfd);
                if (fd >= 0) ::close(fd);
                free(mreply);
                if (merr) free(merr);
                return false;
            }
        }
        free(mreply);
    }
    if (merr) { free(merr); merr = nullptr; }

    if (!haveMulti) {
        // v1.0 fallback: single plane, no modifier (assume linear/invalid).
        auto cookie = xcb_dri3_buffer_from_pixmap(conn, (xcb_pixmap_t)pixmap);
        xcb_generic_error_t* err = nullptr;
        xcb_dri3_buffer_from_pixmap_reply_t* reply =
            xcb_dri3_buffer_from_pixmap_reply(conn, cookie, &err);
        if (!reply) {
            if (err) free(err);
            return false;
        }
        if (reply->nfd >= 1) {
            int* fds = xcb_dri3_buffer_from_pixmap_reply_fds(conn, reply);
            fd     = fds[0];
            stride = reply->stride;
            offset = 0;
            modifier = 0;  // DRM_FORMAT_MOD_LINEAR — best-effort guess for v1.0
        }
        free(reply);
        if (err) free(err);
    }

    if (fd < 0) return false;

    try {
        // ARGB8888 on a 32-bit TrueColor visual (BGRA in memory).
        m_dmaImport = DmaBufImport::importFd(*m_vkCtx, fd, w, h,
                                             0x34325241 /*DRM_FORMAT_ARGB8888*/,
                                             modifier, offset, stride);
    } catch (const std::exception& e) {
        LOG_WARN("X11: DMA-BUF import failed (%s)", e.what());
        ::close(fd);
        return false;
    }
    // importFd dup()'d the fd; close our copy.
    ::close(fd);

    m_dmaImportValid = true;
    m_dmaImportW = (int)w;
    m_dmaImportH = (int)h;
    m_dmaImportPixmap = (uint64_t)pixmap;
    LOG_INFO("X11: DMA-BUF import succeeded (%ux%u, modifier 0x%llx)",
             w, h, (unsigned long long)modifier);
    return true;
}

std::optional<X11SessionFrame> RealX11CaptureSession::grabViaDmaBuf() {
    // Which pixmap holds this frame's pixels: the IncludeInferiors staging
    // pixmap for monitor sources, or the window's backing pixmap.
    Drawable pixmap = needsCompositeCopy() ? m_stagingPixmap : targetDrawable();
    if (pixmap == 0) return std::nullopt;

    if (!ensureDmaImport(pixmap, m_width, m_height)) return std::nullopt;

    // Make sure the server-side copy/render into the pixmap is visible to the
    // GPU import before the consumer samples it this frame.
    XSync(m_display, False);

    X11SessionFrame f;
    f.width          = m_width;
    f.height         = m_height;
    f.fourcc         = 0x34325241;  // ARGB8888 (BGRA in memory)
    f.importedDmaBuf = &m_dmaImport;
    f.modifier       = 0;
    return f;
}
#else
bool RealX11CaptureSession::ensureDmaImport(Drawable, uint32_t, uint32_t) { return false; }
std::optional<X11SessionFrame> RealX11CaptureSession::grabViaDmaBuf() { return std::nullopt; }
#endif

void RealX11CaptureSession::freeDmaImport() {
#if SHADERSCOPE_HAVE_XCB_DRI3
    if (m_dmaImportValid && m_vkCtx) {
        DmaBufImport::destroy(*m_vkCtx, m_dmaImport);
    }
#endif
    m_dmaImportValid = false;
    m_dmaImportW = m_dmaImportH = 0;
    m_dmaImportPixmap = 0;
}
