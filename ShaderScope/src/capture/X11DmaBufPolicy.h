#pragma once
#include <cstdint>

// Pure decision logic for the X11 DRI3 DMA-BUF fast path. Kept free of any
// Xlib/xcb/Vulkan dependency so it can be unit-tested in isolation.
namespace X11DmaBufPolicy {

// Inputs that decide whether grab() should attempt the DMA-BUF fast path.
struct Inputs {
    bool hasVulkanContext = false;  // a VulkanContext* was wired in
    bool vulkanSupportsDmaBuf = false;  // DmaBufImport::isSupported()
    bool dri3Available = false;     // xcb_dri3_query_version succeeded (>= 1.0)
    bool disabledByEnv = false;     // SHADERSCOPE_DISABLE_X11_DMABUF=1
};

// True when every precondition for the DMA-BUF path is met. A false return
// means grab() should use the IncludeInferiors XCopyArea + XShmGetImage CPU
// path (Part 1), which always works.
inline bool shouldUseDmaBuf(const Inputs& in) {
    if (in.disabledByEnv) return false;
    if (!in.hasVulkanContext) return false;
    if (!in.vulkanSupportsDmaBuf) return false;
    if (!in.dri3Available) return false;
    return true;
}

// Parses the SHADERSCOPE_DISABLE_X11_DMABUF env value. Treats "1", "true",
// "yes", "on" (case-insensitive, first char) as "disable". Null/empty → false.
bool envForcesDisable(const char* value);

} // namespace X11DmaBufPolicy
