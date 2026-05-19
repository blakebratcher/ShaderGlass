#include <gtest/gtest.h>
#include "util/ScreenshotPath.h"
#include <filesystem>
#include <fstream>

TEST(ScreenshotPath, UsesPicturesDirWhenSet) {
    auto tmp = std::filesystem::temp_directory_path() / "shaderglass-pics";
    std::filesystem::create_directories(tmp);

    ScreenshotPath::Resolver r;
    r.picturesDirOverride = tmp;

    auto p = r.resolve(std::chrono::system_clock::time_point{});
    EXPECT_EQ(p.parent_path(), tmp);
    EXPECT_TRUE(p.filename().string().starts_with("shaderglass-"));
    EXPECT_TRUE(p.filename().string().ends_with(".png"));
}

TEST(ScreenshotPath, AppendsUniqueSuffixOnCollision) {
    auto tmp = std::filesystem::temp_directory_path() / "shaderglass-pics-coll";
    std::filesystem::remove_all(tmp);
    std::filesystem::create_directories(tmp);

    ScreenshotPath::Resolver r;
    r.picturesDirOverride = tmp;
    auto fixed = std::chrono::system_clock::time_point{} + std::chrono::seconds(42);

    auto p1 = r.resolve(fixed);
    std::ofstream{p1}.put('x');
    auto p2 = r.resolve(fixed);
    EXPECT_NE(p1, p2);
    EXPECT_TRUE(p2.filename().string().find("-001") != std::string::npos
             || p2.filename().string().find("(1)") != std::string::npos);
}

TEST(ScreenshotPath, FallsBackToHomePicturesThenHome) {
    auto fakeHome = std::filesystem::temp_directory_path() / "shaderglass-fake-home";
    std::filesystem::remove_all(fakeHome);
    std::filesystem::create_directories(fakeHome);
    std::filesystem::create_directories(fakeHome / "Pictures");

    ScreenshotPath::Resolver r;
    r.picturesDirOverride.reset();
    r.homeOverride = fakeHome;

    auto p = r.resolve(std::chrono::system_clock::time_point{});
    EXPECT_EQ(p.parent_path(), fakeHome / "Pictures");

    std::filesystem::remove_all(fakeHome / "Pictures");
    auto p2 = r.resolve(std::chrono::system_clock::time_point{});
    EXPECT_EQ(p2.parent_path(), fakeHome);
}
