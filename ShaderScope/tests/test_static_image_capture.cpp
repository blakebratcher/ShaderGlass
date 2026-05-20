#include <gtest/gtest.h>
#include "capture/StaticImageCapture.h"
#include <filesystem>

TEST(StaticImageCapture, LoadsKnown4x4RedPng) {
    StaticImageCapture cap{ std::filesystem::path(TEST_DATA_DIR) / "4x4_red.png" };
    auto sources = cap.enumerateSources();
    ASSERT_EQ(sources.size(), 1u);
    cap.selectSource(sources[0]);

    auto f = cap.acquireFrame();
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(f->kind,   CapturedFrame::Kind::CpuBuffer);
    EXPECT_EQ(f->width,  4u);
    EXPECT_EQ(f->height, 4u);
    EXPECT_GE(f->stride, 4u * 4u);

    // Pixel (0,0) should be RGBA(255, 0, 0, 255).
    ASSERT_NE(f->data, nullptr);
    EXPECT_EQ(f->data[0], 255u);
    EXPECT_EQ(f->data[1], 0u);
    EXPECT_EQ(f->data[2], 0u);
    EXPECT_EQ(f->data[3], 255u);

    cap.release(*f);
}
