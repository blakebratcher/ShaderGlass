#pragma once
#include "../util/SourceInfo.h"
#include <cstdint>
#include <cstddef>
#include <optional>
#include <utility>
#include <vector>

// A single CPU-side frame returned by an X11CaptureSession::grab() call.
// `data` is owned by the session — valid until the next grab() or stop().
struct X11SessionFrame {
    const uint8_t* data   = nullptr;
    size_t         stride = 0;
    uint32_t       fourcc = 0;     // DRM fourcc, e.g. DRM_FORMAT_ARGB8888 (BGRA in memory)
    uint32_t       width  = 0;
    uint32_t       height = 0;
};

// Pull-based capture session for X11 sources (monitors, top-level windows).
// Lifetime contract:
//   - enumerateSources() may be called any time and is idempotent-ish (the
//     real session re-queries the X server on each call).
//   - start(src) must be called before grab(). Calling start() twice is an
//     error.
//   - grab() returns the latest CPU image; pointers are valid until the next
//     grab()/stop().
//   - stop() releases all per-session resources (SHM segment, composite
//     redirection). It is safe to call from the destructor.
//   - All methods must be called from a single thread. The session does not
//     internally synchronize; Xlib is not thread-safe without XInitThreads(),
//     which we do not call.
class X11CaptureSession {
public:
    virtual ~X11CaptureSession() = default;

    virtual std::vector<SourceInfo>           enumerateSources() = 0;
    virtual void                              start(const SourceInfo& source) = 0;
    virtual void                              stop() = 0;
    virtual std::optional<X11SessionFrame>    grab() = 0;

    // Pixel dimensions of the last captured frame (or the session's known
    // source size before the first grab). Returns {0,0} if unknown.
    virtual std::pair<int,int>               size() const { return {0,0}; }
};
