// SPIRV_stub.cpp — Linux build only. SPIRV.cpp converts SPIR-V → HLSL via
// spirv-cross for the Windows DirectX path; the Linux Vulkan target consumes
// SPIR-V directly, so HLSL emission is dead code. This stub exists solely so
// unconditional links succeed; behavior is wired up properly in later tasks
// when the SPIR-V pipeline is exercised.
#include "pch.h"
#include "SPIRV.h"

std::pair<std::string, std::string> SPIRV::GenerateHLSL(const std::vector<uint32_t>& /*bin*/,
                                                        bool /*fragment*/,
                                                        std::ostream& log,
                                                        bool& warn)
{
    log << "[SPIRV_stub] GenerateHLSL called on Linux build — returning empty pair.\n";
    warn = true;
    return {std::string{}, std::string{}};
}
