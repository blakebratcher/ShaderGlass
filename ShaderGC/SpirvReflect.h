/*
ShaderGC: slangp shader compiler for ShaderGlass
Copyright (C) 2021-2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Minimal SPIR-V reflection for the Linux/Vulkan runtime.
//
// The Windows trunk derives parameter/sampler layout from spirv-cross JSON
// metadata (SPIRV.cpp). On Linux, the Vulkan runtime consumes SPIR-V
// directly, so this module parses the binary itself: uniform-block and
// push-constant-block struct members (name + byte offset + byte size),
// combined-image-sampler bindings, and whether the vertex stage declares
// Location-decorated inputs (the RetroArch Position/TexCoord convention).
namespace SpirvReflect
{

struct BlockMember
{
    std::string name;
    uint32_t    offset = 0;
    uint32_t    size   = 0;
};

struct Block
{
    int                      set     = 0;
    int                      binding = -1; // -1 == push constant
    uint32_t                 size    = 0;  // max(member offset + size)
    std::vector<BlockMember> members;
};

struct Sampler
{
    std::string name;
    int         set     = 0;
    int         binding = 0;
};

struct Reflection
{
    std::vector<Block>   uniformBlocks;  // storage class Uniform, Block-decorated
    std::optional<Block> pushConstants;  // storage class PushConstant
    std::vector<Sampler> samplers;       // storage class UniformConstant
    // True when any Input-storage variable carries a Location decoration.
    // Only meaningful for vertex-stage modules (fragment varyings also have
    // locations); callers must check the stage themselves.
    bool                 usesLocationInputs = false;
};

// Parses a SPIR-V module. Returns nullopt when `words` is not valid SPIR-V.
std::optional<Reflection> Reflect(const uint32_t* words, size_t wordCount);

} // namespace SpirvReflect
