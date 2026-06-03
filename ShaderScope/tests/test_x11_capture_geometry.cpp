// Unit tests for the pure X11 capture geometry / cache-key helpers.
// No X server / Vulkan / GPU required — these guard the Bug-1 (stale
// window backing pixmap) and Bug-2 (stale monitor crop after XRandR
// change) staleness fixes.

#include <gtest/gtest.h>
#include "capture/X11CaptureGeometry.h"

using X11CaptureGeometry::cropFitsRoot;
using X11CaptureGeometry::dmaImportCacheValid;

// ---------------------------------------------------------------------------
// cropFitsRoot — Bug 2 belt-and-braces validation.
// ---------------------------------------------------------------------------

TEST(X11CropFitsRoot, ExactFitAtOriginIsValid) {
    EXPECT_TRUE(cropFitsRoot(0, 0, 2560, 1440, 2560, 1440));
}

TEST(X11CropFitsRoot, RightHalfOfDualMonitorIsValid) {
    // Second 2560x1440 monitor at x=2560 within a 5120x1440 root.
    EXPECT_TRUE(cropFitsRoot(2560, 0, 2560, 1440, 5120, 1440));
}

TEST(X11CropFitsRoot, CropWiderThanRootIsRejected) {
    // After the monitor shrank (e.g. 2560 -> 1920) but crop still says 2560.
    EXPECT_FALSE(cropFitsRoot(0, 0, 2560, 1440, 1920, 1440));
}

TEST(X11CropFitsRoot, RightEdgeOverhangIsRejected) {
    // Monitor moved / root shrank: crop's right edge now exceeds root width.
    EXPECT_FALSE(cropFitsRoot(2560, 0, 2560, 1440, 3840, 1440));
}

TEST(X11CropFitsRoot, BottomEdgeOverhangIsRejected) {
    EXPECT_FALSE(cropFitsRoot(0, 1080, 1920, 1080, 1920, 1440));
}

TEST(X11CropFitsRoot, NegativeOriginIsRejected) {
    EXPECT_FALSE(cropFitsRoot(-1, 0, 100, 100, 1920, 1080));
    EXPECT_FALSE(cropFitsRoot(0, -1, 100, 100, 1920, 1080));
}

TEST(X11CropFitsRoot, ZeroAreaIsRejected) {
    EXPECT_FALSE(cropFitsRoot(0, 0, 0, 100, 1920, 1080));
    EXPECT_FALSE(cropFitsRoot(0, 0, 100, 0, 1920, 1080));
}

TEST(X11CropFitsRoot, NoUint32OverflowOnLargeOrigin) {
    // cropX near UINT32_MAX + width must not wrap to a small value and
    // spuriously "fit". (Computed in 64-bit internally.)
    EXPECT_FALSE(cropFitsRoot(2000000000, 0, 2000000000, 100, 4000, 4000));
}

// ---------------------------------------------------------------------------
// dmaImportCacheValid — Bug 1 part 3: key the DMA import on pixmap XID.
// ---------------------------------------------------------------------------

TEST(X11DmaImportCache, ValidWhenPixmapAndDimsMatch) {
    EXPECT_TRUE(dmaImportCacheValid(/*valid*/ true,
                                    /*cachedPixmap*/ 0x4200001, 2560, 1440,
                                    /*pixmap*/ 0x4200001, 2560, 1440));
}

TEST(X11DmaImportCache, InvalidWhenNoImportHeld) {
    EXPECT_FALSE(dmaImportCacheValid(/*valid*/ false,
                                     0x4200001, 2560, 1440,
                                     0x4200001, 2560, 1440));
}

TEST(X11DmaImportCache, InvalidWhenPixmapXidChangedSameDims) {
    // The Bug-1 case: minimize + restore at the same size re-names the
    // backing pixmap, so the XID differs while w/h are unchanged. A
    // dims-only key would wrongly keep the dead import.
    EXPECT_FALSE(dmaImportCacheValid(/*valid*/ true,
                                     /*cachedPixmap*/ 0x4200001, 2560, 1440,
                                     /*pixmap*/ 0x4200099, 2560, 1440));
}

TEST(X11DmaImportCache, InvalidWhenWidthChanged) {
    EXPECT_FALSE(dmaImportCacheValid(true,
                                     0x4200001, 2560, 1440,
                                     0x4200001, 1920, 1440));
}

TEST(X11DmaImportCache, InvalidWhenHeightChanged) {
    EXPECT_FALSE(dmaImportCacheValid(true,
                                     0x4200001, 2560, 1440,
                                     0x4200001, 2560, 1080));
}
