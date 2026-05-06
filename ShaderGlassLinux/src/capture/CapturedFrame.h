#pragma once
#include <cstdint>
#include <cstddef>

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
