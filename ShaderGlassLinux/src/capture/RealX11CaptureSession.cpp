#include "RealX11CaptureSession.h"
#include "util/Logging.h"
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xatom.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

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

bool windowHasHiddenState(Display* d, Window w) {
    Atom netWmState   = XInternAtom(d, "_NET_WM_STATE", False);
    Atom hiddenState  = XInternAtom(d, "_NET_WM_STATE_HIDDEN", False);
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long nitems = 0, bytesAfter = 0;
    unsigned char* data = nullptr;
    bool isHidden = false;
    if (XGetWindowProperty(d, w, netWmState, 0, 128, False, XA_ATOM,
                           &actualType, &actualFormat, &nitems, &bytesAfter,
                           &data) == Success && data) {
        const Atom* atoms = reinterpret_cast<const Atom*>(data);
        for (unsigned long i = 0; i < nitems; ++i) {
            if (atoms[i] == hiddenState) { isHidden = true; break; }
        }
        XFree(data);
    }
    return isHidden;
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

    Atom netWmName = XInternAtom(m_display, "_NET_WM_NAME", False);
    Atom utf8      = XInternAtom(m_display, "UTF8_STRING", False);

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
            if (attr.map_state != IsViewable && !windowHasHiddenState(m_display, w)) {
                // unmapped and not "hidden" (minimized) — skip
                continue;
            }

            std::string name = getStringProp(m_display, w, netWmName, utf8);
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
        XRRScreenResources* res = XRRGetScreenResources(m_display, m_root);
        if (!res) throw std::runtime_error("X11: XRRGetScreenResources failed");
        bool found = false;
        for (int i = 0; i < res->noutput && !found; ++i) {
            XRROutputInfo* oi = XRRGetOutputInfo(m_display, res, res->outputs[i]);
            if (oi && oi->name && m_outputName == oi->name &&
                oi->connection == RR_Connected && oi->crtc) {
                XRRCrtcInfo* ci = XRRGetCrtcInfo(m_display, res, oi->crtc);
                if (ci) {
                    m_cropX  = ci->x;
                    m_cropY  = ci->y;
                    m_width  = ci->width;
                    m_height = ci->height;
                    XRRFreeCrtcInfo(ci);
                    found = true;
                }
            }
            if (oi) XRRFreeOutputInfo(oi);
        }
        XRRFreeScreenResources(res);
        if (!found) throw std::runtime_error("X11: output not found: " + m_outputName);
    } else if (id.rfind("window:", 0) == 0) {
        m_sourceKind = SourceKind::XWindow;
        unsigned long xid = 0;
        if (std::sscanf(id.c_str() + strlen("window:"), "0x%lx", &xid) != 1) {
            throw std::runtime_error("X11: bad window id: " + id);
        }
        m_windowTarget = (Window)xid;

        // Composite-redirect so we can capture even when the window is
        // partially obscured or minimized. The pixmap is the off-screen
        // backing store the composite manager renders into.
        XCompositeRedirectWindow(m_display, m_windowTarget,
                                 CompositeRedirectAutomatic);
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

    allocSharedImage(m_width, m_height);
}

void RealX11CaptureSession::stop() {
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
    m_sourceKind = SourceKind::Unset;
    m_width = m_height = 0;
}

std::optional<X11SessionFrame> RealX11CaptureSession::grab() {
    if (m_sourceKind == SourceKind::Unset) return std::nullopt;

    if (!m_haveXShm) {
        // Slow path covered in Task 14.
        return std::nullopt;
    }

    Drawable d = targetDrawable();
    int srcX = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropX : 0;
    int srcY = (m_sourceKind == SourceKind::MonitorOutput) ? m_cropY : 0;

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
    if (!m_haveXShm) return;   // XGetImage fallback path doesn't need SHM.

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
    m_image->data  = m_shm.shmaddr;
    m_shm.readOnly = False;

    if (!XShmAttach(m_display, &m_shm)) {
        shmdt(m_shm.shmaddr);
        shmctl(m_shm.shmid, IPC_RMID, nullptr);
        XDestroyImage(m_image); m_image = nullptr;
        throw std::runtime_error("X11: XShmAttach failed");
    }
    XSync(m_display, False);  // server-side attach must complete
    // The SHM segment is marked for deletion immediately; it stays alive
    // until the server detaches it on XShmDetach() / process exit.
    shmctl(m_shm.shmid, IPC_RMID, nullptr);
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

bool RealX11CaptureSession::reallocIfDimsChanged(uint32_t, uint32_t) { return true; }

Drawable RealX11CaptureSession::targetDrawable() const {
    if (m_sourceKind == SourceKind::XWindow && m_havePixmap) return m_pixmap;
    return m_root;
}
