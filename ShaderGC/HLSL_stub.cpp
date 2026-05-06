// HLSL_stub.cpp — Linux build only. The Vulkan target consumes SPIR-V
// directly and never goes through fxc, so HLSL emission is dead code on
// Linux. This stub exists solely so unconditional links succeed.
#include "pch.h"
#include "HLSL.h"

std::vector<uint8_t> HLSL::CompileHLSL(const char* /*source*/, size_t /*size*/,
                                       const char* /*profile*/, bool /*unroll*/,
                                       std::ostream& log, bool& warn)
{
    log << "[HLSL_stub] CompileHLSL called on Linux build — returning empty bytecode.\n";
    warn = true;
    return {};
}
