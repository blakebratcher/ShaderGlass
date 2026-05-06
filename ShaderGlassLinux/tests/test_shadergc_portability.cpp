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
