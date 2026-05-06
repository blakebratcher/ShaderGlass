#pragma once
#include <cstdint>
#include <cstddef>

// Lifetime contract:
//   - The frame returned by CaptureBackend::acquireFrame() points into memory
//     owned by the backend. Callers must NOT outlive the backend.
//   - For DMA-BUF frames the buffer must be returned to the source via
//     CaptureBackend::release(frame) before the next acquireFrame() call —
//     PipeWire/X11 backends rely on this. (StaticImageCapture's release()
//     is a no-op, but other backends are not.)
//   - The CapturedFrame struct itself is a small POD and is safe to copy.
struct CapturedFrame {
    enum class Kind { CpuBuffer, DmaBuf };

    Kind          kind   = Kind::CpuBuffer;
    uint32_t      width  = 0;
    uint32_t      height = 0;
    uint32_t      fourcc = 0;        // DRM fourcc (DRM_FORMAT_ABGR8888 etc.)
    uint64_t      modifier = 0;      // DRM format modifier (DmaBuf only)

    // CpuBuffer:
    const uint8_t* data   = nullptr;
    size_t         stride = 0;       // bytes per row

    // DmaBuf:
    int            fd     = -1;
    size_t         offset = 0;
};
