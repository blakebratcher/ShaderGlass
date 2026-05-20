#include <gtest/gtest.h>
#include "util/XdgConfig.h"
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

class XdgConfigTest : public ::testing::Test {
protected:
    fs::path m_tmp;
    void SetUp() override {
        m_tmp = fs::temp_directory_path() / ("xdg_test_" + std::to_string(::getpid()));
        fs::create_directories(m_tmp);
        ::setenv("XDG_CONFIG_HOME", m_tmp.string().c_str(), 1);
    }
    void TearDown() override {
        ::unsetenv("XDG_CONFIG_HOME");
        fs::remove_all(m_tmp);
    }
};

TEST_F(XdgConfigTest, RoundTripsToken) {
    XdgConfig::writeToken("portal-token", "abcdef-0123");
    auto v = XdgConfig::readToken("portal-token");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "abcdef-0123");
}

TEST_F(XdgConfigTest, MissingTokenReturnsNullopt) {
    auto v = XdgConfig::readToken("nope");
    EXPECT_FALSE(v.has_value());
}

TEST_F(XdgConfigTest, FileHas0600Permissions) {
    XdgConfig::writeToken("perms", "x");
    fs::path p = m_tmp / "shaderscope" / "perms";
    auto perms = fs::status(p).permissions();
    EXPECT_EQ(perms & fs::perms::owner_read,  fs::perms::owner_read);
    EXPECT_EQ(perms & fs::perms::owner_write, fs::perms::owner_write);
    EXPECT_EQ(perms & fs::perms::group_all,   fs::perms::none);
    EXPECT_EQ(perms & fs::perms::others_all,  fs::perms::none);
}
