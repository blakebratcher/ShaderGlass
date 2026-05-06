// Verifies that on Linux, ShaderGC::CompilePreset emits valid SPIR-V binary
// (magic number 0x07230203) into ShaderDef bytecode fields, bypassing the
// SPIR-V → HLSL → DXBC translation that the Windows build performs.

#include <gtest/gtest.h>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <cstdint>
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"

namespace fs = std::filesystem;

static constexpr uint32_t SPIRV_MAGIC = 0x07230203u;

TEST(ShaderGCSpirv, CompilePresetEmitsValidSpirvOnLinux) {
    fs::path data = fs::path(TEST_DATA_DIR) / "stock.slangp";
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;

    PresetDef* preset = ShaderGC::CompilePreset(data, log, warn, cache);
    ASSERT_NE(preset, nullptr) << log.str();
    ASSERT_FALSE(preset->ShaderDefs.empty()) << "preset has no shaders\nlog: " << log.str();

    auto& shader = preset->ShaderDefs[0];
    ASSERT_GE(shader.VertexLength,   4u) << "log: " << log.str();
    ASSERT_GE(shader.FragmentLength, 4u) << "log: " << log.str();
    ASSERT_NE(shader.VertexByteCode,   nullptr);
    ASSERT_NE(shader.FragmentByteCode, nullptr);

    uint32_t vertexMagic = 0, fragmentMagic = 0;
    std::memcpy(&vertexMagic,   shader.VertexByteCode,   4);
    std::memcpy(&fragmentMagic, shader.FragmentByteCode, 4);

    EXPECT_EQ(vertexMagic,   SPIRV_MAGIC) << "vertex stage did not emit SPIR-V";
    EXPECT_EQ(fragmentMagic, SPIRV_MAGIC) << "fragment stage did not emit SPIR-V";

    preset->MakeDynamic();   // flips Dynamic=true so the destructor frees the malloc'd SPIR-V buffers
    delete preset;
}
