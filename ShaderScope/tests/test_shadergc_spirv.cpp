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

// ── Semantic / param reflection (Linux SPIR-V path) ─────────────────────────
//
// RetroArch slang shaders put their built-in semantics (MVP, SourceSize,
// OutputSize, OriginalSize, FrameCount, FinalViewportSize, FrameDirection)
// and their #pragma parameters inside a UBO (buffer >= 0 == binding) and/or
// the push_constant block (buffer == -1). The runtime can only write values
// at the right place if ShaderGC reflects each member's offset/size from the
// compiled SPIR-V. These tests pin that contract using the real starter
// shaders that ship with ShaderScope.

namespace {

struct CompiledPreset {
    PresetDef* def = nullptr;
    ~CompiledPreset() {
        if (def) {
            def->MakeDynamic();
            delete def;
        }
    }
};

PresetDef* compile(const fs::path& slangp, std::ostream& log) {
    bool warn = false;
    ShaderCache cache;
    return ShaderGC::CompilePreset(slangp, log, warn, cache);
}

const ShaderParam* findParam(ShaderDef& sd, const std::string& name) {
    for (auto& p : sd.Params) {
        if (p.name == name) return &p;
    }
    return nullptr;
}

const ShaderSampler* findSampler(ShaderDef& sd, const std::string& name) {
    for (auto& s : sd.Samplers) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

} // namespace

TEST(ShaderGCReflection, StarterPassthroughReflectsMvpInUbo) {
    fs::path slangp = fs::path(STARTER_SHADERS_DIR) / "passthrough.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    ASSERT_EQ(p.def->ShaderDefs.size(), 1u);
    auto& sd = p.def->ShaderDefs[0];

    // UBO { mat4 MVP; } at set 0, binding 0 → buffer 0, offset 0, 64 bytes.
    const ShaderParam* mvp = findParam(sd, "MVP");
    ASSERT_NE(mvp, nullptr) << "MVP param missing";
    EXPECT_EQ(mvp->buffer, 0)  << "MVP should live in UBO binding 0";
    EXPECT_EQ(mvp->offset, 0)  << "MVP should sit at offset 0";
    EXPECT_EQ(mvp->size,  64)  << "MVP is a mat4 (64 bytes)";

    // The UBO's total size feeds the Vulkan buffer allocation.
    EXPECT_EQ(sd.ParamsSize(0), 64u);
}

TEST(ShaderGCReflection, StarterPassthroughDetectsVertexInput) {
    fs::path slangp = fs::path(STARTER_SHADERS_DIR) / "passthrough.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    // Vertex stage declares `layout(location=0) in vec4 Position` +
    // `layout(location=1) in vec2 TexCoord` → runtime must bind a quad VBO.
    EXPECT_TRUE(sd.UsesVertexInput);
}

TEST(ShaderGCReflection, StarterPassthroughReflectsSourceSamplerBinding) {
    fs::path slangp = fs::path(STARTER_SHADERS_DIR) / "passthrough.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    // passthrough.slang: layout(set = 0, binding = 2) uniform sampler2D Source;
    const ShaderSampler* src = findSampler(sd, "Source");
    ASSERT_NE(src, nullptr) << "Source sampler missing from reflection";
    EXPECT_EQ(src->binding, 2);
}

TEST(ShaderGCReflection, StockShaderHasNoVertexInput) {
    fs::path slangp = fs::path(TEST_DATA_DIR) / "stock.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    // stock.slang synthesises positions from gl_VertexIndex — no vertex
    // inputs. The runtime must keep the no-VBO fullscreen-triangle path.
    EXPECT_FALSE(sd.UsesVertexInput);

    // stock.slang: layout(set = 0, binding = 0) uniform sampler2D Source;
    const ShaderSampler* src = findSampler(sd, "Source");
    ASSERT_NE(src, nullptr);
    EXPECT_EQ(src->binding, 0);
}

TEST(ShaderGCReflection, CrtEasymodeReflectsUboSemanticsAndPushParams) {
    fs::path slangp = fs::path(STARTER_SHADERS_DIR) / "crt-easymode.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    // UBO { mat4 MVP; vec4 OutputSize; vec4 OriginalSize; vec4 SourceSize; }
    struct Expect { const char* name; int offset; int size; };
    const Expect uboMembers[] = {
        {"MVP",          0, 64},
        {"OutputSize",  64, 16},
        {"OriginalSize",80, 16},
        {"SourceSize",  96, 16},
    };
    for (const auto& e : uboMembers) {
        const ShaderParam* prm = findParam(sd, e.name);
        ASSERT_NE(prm, nullptr) << e.name << " missing";
        EXPECT_EQ(prm->buffer, 0)        << e.name << " should be in UBO binding 0";
        EXPECT_EQ(prm->offset, e.offset) << e.name << " offset wrong";
        EXPECT_EQ(prm->size,   e.size)   << e.name << " size wrong";
    }
    EXPECT_EQ(sd.ParamsSize(0), 112u);

    // push_constant Push { float BRIGHT_BOOST; ... float SHARPNESS_V; }
    // 17 floats declared in order → consecutive 4-byte offsets.
    const Expect pushMembers[] = {
        {"BRIGHT_BOOST",        0, 4},
        {"DILATION",            4, 4},
        {"SCANLINE_STRENGTH",  56, 4},
        {"SHARPNESS_H",        60, 4},
        {"SHARPNESS_V",        64, 4},
    };
    for (const auto& e : pushMembers) {
        const ShaderParam* prm = findParam(sd, e.name);
        ASSERT_NE(prm, nullptr) << e.name << " missing";
        EXPECT_EQ(prm->buffer, -1)       << e.name << " should be a push constant";
        EXPECT_EQ(prm->offset, e.offset) << e.name << " offset wrong";
        EXPECT_EQ(prm->size,   e.size)   << e.name << " size wrong";
    }
    EXPECT_EQ(sd.ParamsSize(-1), 68u);

    // User params keep their #pragma parameter metadata (range + default).
    const ShaderParam* sharp = findParam(sd, "SHARPNESS_H");
    ASSERT_NE(sharp, nullptr);
    EXPECT_FLOAT_EQ(sharp->defaultValue, 0.5f);
    EXPECT_FLOAT_EQ(sharp->minValue,     0.0f);
    EXPECT_FLOAT_EQ(sharp->maxValue,     1.0f);
    EXPECT_TRUE(sd.UsesVertexInput);
}

TEST(ShaderGCReflection, NightModeReflectsFinalViewportSize) {
    fs::path slangp = fs::path(STARTER_SHADERS_DIR) / "night-mode.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    // night-mode.slang UBO: mat4 MVP; vec4 FinalViewportSize; vec4 OutputSize;
    const ShaderParam* fvs = findParam(sd, "FinalViewportSize");
    ASSERT_NE(fvs, nullptr) << "FinalViewportSize semantic missing";
    EXPECT_EQ(fvs->buffer, 0);
    EXPECT_EQ(fvs->offset, 64);
    EXPECT_EQ(fvs->size,   16);
}

TEST(ShaderGCReflection, UboAtNonZeroBindingIsNormalisedToBufferZero) {
    // A UBO at binding != 0 (legal, unconventional) must still produce
    // buffer-0 params (so ParamsSize(0)/updateUbo work) and report its real
    // binding via UboBinding for the descriptor write.
    fs::path slangp = fs::path(TEST_DATA_DIR) / "binding1.slangp";
    std::ostringstream log;
    CompiledPreset p{compile(slangp, log)};
    ASSERT_NE(p.def, nullptr) << log.str();
    auto& sd = p.def->ShaderDefs[0];

    const ShaderParam* mvp = findParam(sd, "MVP");
    ASSERT_NE(mvp, nullptr);
    EXPECT_EQ(mvp->buffer, 0) << "UBO members must normalise to buffer 0";
    EXPECT_EQ(mvp->offset, 0);
    EXPECT_EQ(mvp->size,  64);
    EXPECT_EQ(sd.ParamsSize(0), 64u);
    EXPECT_EQ(sd.UboBinding, 1) << "real descriptor binding must be preserved";

    // The conventional layout still reports binding 0.
    fs::path conventional = fs::path(STARTER_SHADERS_DIR) / "passthrough.slangp";
    std::ostringstream log2;
    CompiledPreset p2{compile(conventional, log2)};
    ASSERT_NE(p2.def, nullptr) << log2.str();
    EXPECT_EQ(p2.def->ShaderDefs[0].UboBinding, 0);
}
