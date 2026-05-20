#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstring>
#include <iterator>

namespace fs = std::filesystem;

static std::vector<uint8_t> readFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

TEST(HeadlessRender, PassthroughOnKnown4x4ProducesReferenceOutput) {
    fs::path bin = fs::path(SHADERSCOPE_BIN);
    fs::path in  = fs::path(TEST_DATA_DIR) / "4x4_red.png";
    fs::path out = fs::temp_directory_path() / "shaderscope_headless_out.png";
    fs::path ref = fs::path(TEST_DATA_DIR) / "reference_passthrough_4x4.png";

    fs::remove(out);

    std::string cmd = bin.string() + " --headless"
                    + " --input "  + in.string()
                    + " --output " + out.string()
                    + " --width 4 --height 4";
    int rc = std::system(cmd.c_str());
    ASSERT_EQ(rc, 0) << "headless run failed";
    ASSERT_TRUE(fs::exists(out));

    auto a = readFile(out);
    auto b = readFile(ref);
    ASSERT_EQ(a.size(), b.size()) << "output PNG size differs from reference";
    EXPECT_EQ(0, std::memcmp(a.data(), b.data(), a.size())) << "output PNG bytes differ from reference";
}
