// These tests verify that ShaderGC headers compile and link under GCC/Clang
// with Portability.h active. The __declspec(noinline) shim is exercised
// implicitly (AddParam/AddSampler are annotated with it); the strcpy_s and
// _stricmp shims are covered by ShaderGC.cpp's call sites and are exercised
// by later integration tests in this milestone.

#include <gtest/gtest.h>
#include "ShaderDef.h"
#include "PresetDef.h"

TEST(ShaderGCPortability, ShaderDefAddParamCompilesAndWorks) {
    ShaderDef def;
    def.AddParam("test_param", 0, 0, 4, 0.0f, 1.0f, 0.5f);
    ASSERT_EQ(def.Params.size(), 1u);
    EXPECT_EQ(def.Params[0].name, "test_param");
    EXPECT_FLOAT_EQ(def.Params[0].defaultValue, 0.5f);
}

TEST(ShaderGCPortability, ShaderDefAddSamplerCompilesAndWorks) {
    ShaderDef def;
    def.AddSampler("Source", 0);
    ASSERT_EQ(def.Samplers.size(), 1u);
    EXPECT_EQ(def.Samplers[0].name, "Source");
}
