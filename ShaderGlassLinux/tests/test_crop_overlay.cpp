#include <gtest/gtest.h>
#include "ui/CropOverlay.h"

TEST(CropOverlay, MapViewportPointToSourcePixel_NoLetterbox) {
    auto p = CropOverlay::mapToSource(
        {480, 270},
        {0, 0, 960, 540},
        {1920, 1080});
    ASSERT_TRUE(p.has_value());
    EXPECT_EQ(p->x, 960);
    EXPECT_EQ(p->y, 540);
}

TEST(CropOverlay, MapViewportPointToSourcePixel_Letterbox) {
    auto p = CropOverlay::mapToSource(
        {500, 400},
        {0, 0, 1000, 800},
        {1920, 1080});
    ASSERT_TRUE(p.has_value());
    EXPECT_NEAR(p->x, 960, 1);
    EXPECT_NEAR(p->y, 540, 1);
}

TEST(CropOverlay, MapViewportPointReturnsNulloptOutsideImageRect) {
    auto p = CropOverlay::mapToSource(
        {500, 10},
        {0, 0, 1000, 800},
        {1920, 1080});
    EXPECT_FALSE(p.has_value());
}

TEST(CropOverlay, SnapsTinyRectTo16x16AroundDragMidpoint) {
    CropRect r = CropOverlay::buildRect({100, 100}, {105, 105}, {1920, 1080});
    EXPECT_EQ(r.w, 16);
    EXPECT_EQ(r.h, 16);
    EXPECT_EQ(r.x, 94);
    EXPECT_EQ(r.y, 94);
}

TEST(CropOverlay, BuildRectFromTwoPointsNormalizesXY) {
    CropRect r = CropOverlay::buildRect({500, 600}, {100, 200}, {1920, 1080});
    EXPECT_EQ(r.x, 100);
    EXPECT_EQ(r.y, 200);
    EXPECT_EQ(r.w, 400);
    EXPECT_EQ(r.h, 400);
}
