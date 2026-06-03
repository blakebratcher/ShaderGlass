/*
ShaderGC: slangp shader compiler for ShaderGlass
Copyright (C) 2021-2025 mausimus (mausimus.net)
https://github.com/mausimus/ShaderGlass
GNU General Public License v3.0
*/

#include "SpirvReflect.h"

#include <map>
#include <utility>

namespace SpirvReflect
{

namespace
{

// SPIR-V opcodes (https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)
constexpr uint32_t OpName            = 5;
constexpr uint32_t OpMemberName      = 6;
constexpr uint32_t OpTypeInt         = 21;
constexpr uint32_t OpTypeFloat       = 22;
constexpr uint32_t OpTypeVector      = 23;
constexpr uint32_t OpTypeMatrix      = 24;
constexpr uint32_t OpTypeImage       = 25;
constexpr uint32_t OpTypeSampledImage = 27;
constexpr uint32_t OpTypeArray       = 28;
constexpr uint32_t OpTypeStruct      = 30;
constexpr uint32_t OpTypePointer     = 32;
constexpr uint32_t OpConstant        = 43;
constexpr uint32_t OpVariable        = 59;
constexpr uint32_t OpDecorate        = 71;
constexpr uint32_t OpMemberDecorate  = 72;

// Decorations
constexpr uint32_t DecorationBlock         = 2;
constexpr uint32_t DecorationArrayStride   = 6;
constexpr uint32_t DecorationBuiltIn       = 11;
constexpr uint32_t DecorationLocation      = 30;
constexpr uint32_t DecorationBinding       = 33;
constexpr uint32_t DecorationDescriptorSet = 34;
constexpr uint32_t DecorationOffset        = 35;

// Storage classes
constexpr uint32_t StorageUniformConstant = 0;
constexpr uint32_t StorageInput           = 1;
constexpr uint32_t StorageUniform         = 2;
constexpr uint32_t StoragePushConstant    = 9;

constexpr uint32_t kSpirvMagic = 0x07230203u;

struct TypeInfo
{
    uint32_t              opcode      = 0;
    uint32_t              width       = 0; // OpTypeInt / OpTypeFloat
    uint32_t              componentId = 0; // vector/matrix/array element, sampled-image image
    uint32_t              count       = 0; // vector components / matrix columns / array length id
    uint32_t              arrayStride = 0; // ArrayStride decoration (arrays inside blocks)
    std::vector<uint32_t> memberTypeIds;   // OpTypeStruct
};

struct VarInfo
{
    uint32_t typeId       = 0; // pointer type
    uint32_t storageClass = 0;
};

struct Decorations
{
    int  set         = -1;
    int  binding     = -1;
    int  location    = -1;
    bool hasBuiltIn  = false;
    bool isBlock     = false;
};

std::string readString(const uint32_t* words, size_t firstWord, size_t lastWord)
{
    // SPIR-V strings are nul-terminated UTF-8 packed little-endian in words.
    std::string s;
    for(size_t w = firstWord; w < lastWord; ++w)
    {
        const uint32_t word = words[w];
        for(int b = 0; b < 4; ++b)
        {
            const char c = static_cast<char>((word >> (8 * b)) & 0xFFu);
            if(c == '\0')
                return s;
            s += c;
        }
    }
    return s;
}

class Parser
{
public:
    Parser(const uint32_t* words, size_t wordCount) : m_words(words), m_count(wordCount) { }

    bool parse()
    {
        if(m_count < 5 || m_words[0] != kSpirvMagic)
            return false;

        size_t i = 5;
        while(i < m_count)
        {
            const uint32_t header    = m_words[i];
            const uint32_t wordCount = header >> 16;
            const uint32_t opcode    = header & 0xFFFFu;
            if(wordCount == 0 || i + wordCount > m_count)
                return false;
            handle(opcode, &m_words[i], wordCount);
            i += wordCount;
        }
        return true;
    }

    Reflection build()
    {
        Reflection r;

        for(const auto& [id, var] : m_variables)
        {
            auto ptrIt = m_pointers.find(var.typeId);
            const uint32_t pointeeId = (ptrIt != m_pointers.end()) ? ptrIt->second.second : 0;
            const Decorations deco   = decorationsFor(id);

            switch(var.storageClass)
            {
                case StorageUniform:
                {
                    // UBO: pointee struct decorated Block.
                    if(decorationsFor(pointeeId).isBlock || isStruct(pointeeId))
                    {
                        Block b      = buildBlock(pointeeId);
                        b.set        = (deco.set >= 0) ? deco.set : 0;
                        b.binding    = (deco.binding >= 0) ? deco.binding : 0;
                        r.uniformBlocks.push_back(std::move(b));
                    }
                    break;
                }
                case StoragePushConstant:
                {
                    Block b   = buildBlock(pointeeId);
                    b.set     = 0;
                    b.binding = -1;
                    if(r.pushConstants)
                    {
                        // Merge (defensive; a single module has at most one).
                        for(auto& m : b.members)
                            addMember(*r.pushConstants, m);
                    }
                    else
                    {
                        r.pushConstants = std::move(b);
                    }
                    break;
                }
                case StorageUniformConstant:
                {
                    // Combined image samplers (LUTs, Source, Original, ...).
                    Sampler s;
                    auto nit = m_names.find(id);
                    if(nit == m_names.end())
                        break;
                    s.name    = nit->second;
                    s.set     = (deco.set >= 0) ? deco.set : 0;
                    s.binding = (deco.binding >= 0) ? deco.binding : 0;
                    r.samplers.push_back(std::move(s));
                    break;
                }
                case StorageInput:
                {
                    if(deco.location >= 0 && !deco.hasBuiltIn)
                        r.usesLocationInputs = true;
                    break;
                }
                default:
                    break;
            }
        }
        return r;
    }

private:
    void handle(uint32_t opcode, const uint32_t* inst, uint32_t wordCount)
    {
        switch(opcode)
        {
            case OpName:
                if(wordCount >= 3)
                    m_names[inst[1]] = readString(inst, 2, wordCount);
                break;
            case OpMemberName:
                if(wordCount >= 4)
                    m_memberNames[{inst[1], inst[2]}] = readString(inst, 3, wordCount);
                break;
            case OpDecorate:
                if(wordCount >= 3)
                {
                    Decorations& d = m_decorations[inst[1]];
                    const uint32_t deco = inst[2];
                    const uint32_t val  = (wordCount >= 4) ? inst[3] : 0;
                    if(deco == DecorationBinding)            d.binding  = static_cast<int>(val);
                    else if(deco == DecorationDescriptorSet) d.set      = static_cast<int>(val);
                    else if(deco == DecorationLocation)      d.location = static_cast<int>(val);
                    else if(deco == DecorationBuiltIn)       d.hasBuiltIn = true;
                    else if(deco == DecorationBlock)         d.isBlock  = true;
                    else if(deco == DecorationArrayStride)   m_types[inst[1]].arrayStride = val;
                }
                break;
            case OpMemberDecorate:
                if(wordCount >= 5 && inst[3] == DecorationOffset)
                    m_memberOffsets[{inst[1], inst[2]}] = inst[4];
                else if(wordCount >= 4 && inst[3] == DecorationBuiltIn)
                    m_structHasBuiltins.insert(std::make_pair(inst[1], true));
                break;
            case OpTypeInt:
            case OpTypeFloat:
                if(wordCount >= 3)
                {
                    TypeInfo& t = m_types[inst[1]];
                    t.opcode = opcode;
                    t.width  = inst[2];
                }
                break;
            case OpTypeVector:
            case OpTypeMatrix:
                if(wordCount >= 4)
                {
                    TypeInfo& t   = m_types[inst[1]];
                    t.opcode      = opcode;
                    t.componentId = inst[2];
                    t.count       = inst[3];
                }
                break;
            case OpTypeArray:
                if(wordCount >= 4)
                {
                    TypeInfo& t   = m_types[inst[1]];
                    t.opcode      = opcode;
                    t.componentId = inst[2];
                    t.count       = inst[3]; // id of a constant, resolved later
                }
                break;
            case OpTypeImage:
            case OpTypeSampledImage:
                {
                    TypeInfo& t = m_types[inst[1]];
                    t.opcode    = opcode;
                    if(opcode == OpTypeSampledImage && wordCount >= 3)
                        t.componentId = inst[2];
                }
                break;
            case OpTypeStruct:
                {
                    TypeInfo& t = m_types[inst[1]];
                    t.opcode    = opcode;
                    for(uint32_t m = 2; m < wordCount; ++m)
                        t.memberTypeIds.push_back(inst[m]);
                }
                break;
            case OpTypePointer:
                if(wordCount >= 4)
                    m_pointers[inst[1]] = {inst[2], inst[3]};
                break;
            case OpConstant:
                if(wordCount >= 4)
                    m_constants[inst[2]] = inst[3];
                break;
            case OpVariable:
                if(wordCount >= 4)
                    m_variables[inst[2]] = {inst[1], inst[3]};
                break;
            default:
                break;
        }
    }

    Decorations decorationsFor(uint32_t id) const
    {
        auto it = m_decorations.find(id);
        return (it != m_decorations.end()) ? it->second : Decorations{};
    }

    bool isStruct(uint32_t typeId) const
    {
        auto it = m_types.find(typeId);
        return it != m_types.end() && it->second.opcode == OpTypeStruct;
    }

    uint32_t typeSize(uint32_t typeId) const
    {
        auto it = m_types.find(typeId);
        if(it == m_types.end())
            return 0;
        const TypeInfo& t = it->second;
        switch(t.opcode)
        {
            case OpTypeInt:
            case OpTypeFloat:
                return t.width / 8;
            case OpTypeVector:
                return typeSize(t.componentId) * t.count;
            case OpTypeMatrix:
                // Column-major, tightly packed columns (std140 mat4 = 4 * vec4).
                return typeSize(t.componentId) * t.count;
            case OpTypeArray:
            {
                uint32_t length = 1;
                auto cit = m_constants.find(t.count);
                if(cit != m_constants.end())
                    length = cit->second;
                const uint32_t stride = (t.arrayStride > 0) ? t.arrayStride : typeSize(t.componentId);
                return stride * length;
            }
            case OpTypeStruct:
            {
                uint32_t maxEnd = 0;
                for(size_t m = 0; m < t.memberTypeIds.size(); ++m)
                {
                    auto oit = m_memberOffsets.find({typeId, static_cast<uint32_t>(m)});
                    const uint32_t off = (oit != m_memberOffsets.end()) ? oit->second : 0;
                    const uint32_t end = off + typeSize(t.memberTypeIds[m]);
                    if(end > maxEnd)
                        maxEnd = end;
                }
                return maxEnd;
            }
            default:
                return 0;
        }
    }

    Block buildBlock(uint32_t structTypeId) const
    {
        Block b;
        auto it = m_types.find(structTypeId);
        if(it == m_types.end() || it->second.opcode != OpTypeStruct)
            return b;

        const TypeInfo& t = it->second;
        for(size_t m = 0; m < t.memberTypeIds.size(); ++m)
        {
            BlockMember member;
            auto nit = m_memberNames.find({structTypeId, static_cast<uint32_t>(m)});
            if(nit != m_memberNames.end())
                member.name = nit->second;
            auto oit = m_memberOffsets.find({structTypeId, static_cast<uint32_t>(m)});
            member.offset = (oit != m_memberOffsets.end()) ? oit->second : 0;
            member.size   = typeSize(t.memberTypeIds[m]);
            const uint32_t end = member.offset + member.size;
            if(end > b.size)
                b.size = end;
            b.members.push_back(std::move(member));
        }
        return b;
    }

    static void addMember(Block& block, const BlockMember& m)
    {
        for(const auto& existing : block.members)
        {
            if(existing.name == m.name)
                return;
        }
        block.members.push_back(m);
        if(m.offset + m.size > block.size)
            block.size = m.offset + m.size;
    }

    const uint32_t* m_words;
    size_t          m_count;

    std::map<uint32_t, std::string>                        m_names;
    std::map<std::pair<uint32_t, uint32_t>, std::string>   m_memberNames;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t>      m_memberOffsets;
    std::map<uint32_t, bool>                               m_structHasBuiltins;
    std::map<uint32_t, Decorations>                        m_decorations;
    std::map<uint32_t, TypeInfo>                           m_types;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>>      m_pointers; // id -> (storage, pointee)
    std::map<uint32_t, uint32_t>                           m_constants;
    std::map<uint32_t, VarInfo>                            m_variables;
};

} // namespace

std::optional<Reflection> Reflect(const uint32_t* words, size_t wordCount)
{
    Parser parser(words, wordCount);
    if(!parser.parse())
        return std::nullopt;
    return parser.build();
}

} // namespace SpirvReflect
