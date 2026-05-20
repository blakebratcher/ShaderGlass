// Allocates a small DMA-BUF via gbm, imports it through DmaBufImport, then
// asserts the resulting VkImage has the expected size and format. This test
// is skipped on systems where DMA-BUF Vulkan import isn't supported (some
// llvmpipe configurations, headless CI, drivers without the required
// extensions).

#include <gtest/gtest.h>

#if __has_include(<gbm.h>)
#  include <gbm.h>
#  include <fcntl.h>
#  include <unistd.h>
#  define HAVE_GBM 1
#else
#  define HAVE_GBM 0
#endif

#include "render/VulkanContext.h"
#include "render/DmaBufImport.h"

#if HAVE_GBM
TEST(DmaBufImport, ImportsGbmAllocatedBuffer) {
    VulkanContext ctx({.headless = true, .enableValidation = true});
    if (!DmaBufImport::isSupported(ctx)) {
        GTEST_SKIP() << "VK_EXT_external_memory_dma_buf or "
                        "VK_EXT_image_drm_format_modifier not available";
    }

    int drmFd = ::open("/dev/dri/renderD128", O_RDWR);
    if (drmFd < 0) GTEST_SKIP() << "no DRM render node available";
    gbm_device* gbm = gbm_create_device(drmFd);
    if (!gbm) {
        ::close(drmFd);
        GTEST_SKIP() << "gbm_create_device failed";
    }

    constexpr uint32_t W = 16, H = 16;
    uint32_t fourcc = 0x34324241; // ABGR8888
    gbm_bo* bo = gbm_bo_create(gbm, W, H, fourcc, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!bo) {
        gbm_device_destroy(gbm); ::close(drmFd);
        GTEST_SKIP() << "gbm_bo_create failed (linear ABGR8888 unsupported)";
    }

    int dmaFd      = gbm_bo_get_fd(bo);
    uint32_t stride = gbm_bo_get_stride(bo);
    uint64_t modifier = gbm_bo_get_modifier(bo);

    auto imp = DmaBufImport::importFd(ctx, dmaFd, W, H, fourcc, modifier, 0, stride);
    EXPECT_NE(imp.image,  VK_NULL_HANDLE);
    EXPECT_NE(imp.view,   VK_NULL_HANDLE);
    EXPECT_NE(imp.memory, VK_NULL_HANDLE);
    EXPECT_EQ(imp.width,  W);
    EXPECT_EQ(imp.height, H);
    DmaBufImport::destroy(ctx, imp);

    ::close(dmaFd);
    gbm_bo_destroy(bo);
    gbm_device_destroy(gbm);
    ::close(drmFd);
}
#else
TEST(DmaBufImport, ImportsGbmAllocatedBuffer) {
    GTEST_SKIP() << "gbm headers not available at build time";
}
#endif
