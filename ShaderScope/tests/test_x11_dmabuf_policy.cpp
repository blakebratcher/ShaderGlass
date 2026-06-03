// Unit tests for the pure X11 DRI3 DMA-BUF fast-path decision logic.
// No X server / Vulkan / GPU required.

#include <gtest/gtest.h>
#include "capture/X11DmaBufPolicy.h"

using X11DmaBufPolicy::Inputs;
using X11DmaBufPolicy::shouldUseDmaBuf;
using X11DmaBufPolicy::envForcesDisable;

namespace {
Inputs allGood() {
    Inputs in;
    in.hasVulkanContext     = true;
    in.vulkanSupportsDmaBuf = true;
    in.dri3Available        = true;
    in.disabledByEnv        = false;
    return in;
}
} // namespace

TEST(X11DmaBufPolicy, AllPreconditionsMetEnablesFastPath) {
    EXPECT_TRUE(shouldUseDmaBuf(allGood()));
}

TEST(X11DmaBufPolicy, EnvDisableForcesCpuPath) {
    Inputs in = allGood();
    in.disabledByEnv = true;
    EXPECT_FALSE(shouldUseDmaBuf(in));
}

TEST(X11DmaBufPolicy, MissingVulkanContextFallsBack) {
    Inputs in = allGood();
    in.hasVulkanContext = false;
    EXPECT_FALSE(shouldUseDmaBuf(in));
}

TEST(X11DmaBufPolicy, VulkanWithoutDmaBufSupportFallsBack) {
    Inputs in = allGood();
    in.vulkanSupportsDmaBuf = false;
    EXPECT_FALSE(shouldUseDmaBuf(in));
}

TEST(X11DmaBufPolicy, NoDri3FallsBack) {
    Inputs in = allGood();
    in.dri3Available = false;
    EXPECT_FALSE(shouldUseDmaBuf(in));
}

TEST(X11DmaBufPolicy, EnvDisableWinsOverEverything) {
    // Even with every capability present, the env override forces CPU.
    Inputs in = allGood();
    in.disabledByEnv = true;
    EXPECT_FALSE(shouldUseDmaBuf(in));
}

TEST(X11DmaBufPolicy, EnvForcesDisableTruthyValues) {
    EXPECT_TRUE(envForcesDisable("1"));
    EXPECT_TRUE(envForcesDisable("true"));
    EXPECT_TRUE(envForcesDisable("TRUE"));
    EXPECT_TRUE(envForcesDisable("yes"));
    EXPECT_TRUE(envForcesDisable("Yes"));
    EXPECT_TRUE(envForcesDisable("on"));
    EXPECT_TRUE(envForcesDisable("ON"));
}

TEST(X11DmaBufPolicy, EnvForcesDisableFalsyValues) {
    EXPECT_FALSE(envForcesDisable(nullptr));
    EXPECT_FALSE(envForcesDisable(""));
    EXPECT_FALSE(envForcesDisable("0"));
    EXPECT_FALSE(envForcesDisable("false"));
    EXPECT_FALSE(envForcesDisable("no"));
    EXPECT_FALSE(envForcesDisable("off"));
    EXPECT_FALSE(envForcesDisable("OFF"));
}
