#pragma once
#include <cstdint>

// Pure geometry / cache-key decision helpers for RealX11CaptureSession.
// Kept free of any Xlib/xcb/Vulkan dependency so they can be unit-tested in
// isolation (mirrors X11DmaBufPolicy.h).
namespace X11CaptureGeometry {

// True when the cropped region [cropX, cropX+w) x [cropY, cropY+h) fits
// entirely within a root of size rootW x rootH (and is non-degenerate).
//
// Used by the MonitorOutput grab() path as a belt-and-braces validation: if
// the crop no longer fits the current root (because an XRandR mode/layout
// change moved or shrank the screen and we missed the event), XCopyArea would
// read partially out of bounds and silently leave stale pixels in the
// uncovered region. A false return means "drop this frame and re-query".
//
// Negative origins or a zero-area crop are treated as not fitting — they are
// never valid capture rectangles.
inline bool cropFitsRoot(int cropX, int cropY,
                         uint32_t w, uint32_t h,
                         uint32_t rootW, uint32_t rootH) {
    if (w == 0 || h == 0) return false;
    if (cropX < 0 || cropY < 0) return false;
    // Compare in 64-bit to avoid uint32 overflow on cropX + w.
    const uint64_t right  = static_cast<uint64_t>(cropX) + w;
    const uint64_t bottom = static_cast<uint64_t>(cropY) + h;
    return right <= rootW && bottom <= rootH;
}

// True when a cached DMA-BUF import is still valid for the pixmap + dimensions
// being requested this grab. A false return means grab() must re-import (the
// pixmap was re-named after a map/unmap/reconfigure, or the size changed).
//
//   valid       — m_dmaImportValid: an import is currently held
//   cachedPixmap/cachedW/cachedH — the (XID, w, h) the held import was built for
//   pixmap/w/h  — what this grab needs
//
// Keying on the pixmap XID (not just w/h) is the Bug-1 fix: after a window is
// minimized + restored at the SAME size, XCompositeNameWindowPixmap returns a
// NEW backing pixmap XID, so a w/h-only key would wrongly keep the dead import.
inline bool dmaImportCacheValid(bool valid,
                                uint64_t cachedPixmap, int cachedW, int cachedH,
                                uint64_t pixmap, int w, int h) {
    return valid &&
           cachedPixmap == pixmap &&
           cachedW == w &&
           cachedH == h;
}

} // namespace X11CaptureGeometry
