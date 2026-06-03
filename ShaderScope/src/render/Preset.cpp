#include "Preset.h"
#include "TextureDef.h"
#include "VulkanContext.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include "util/Logging.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace {

// RetroArch slang per-pass scaling.
struct PassScale {
    enum class Type : int { Source = 0, Viewport, Absolute };
    Type  type   = Type::Source;
    float factor = 1.0f;
};

PassScale parseScale(const std::map<std::string, std::string>& pp, char axis) {
    PassScale s;
    auto find = [&](const std::string& key) -> const std::string* {
        auto it = pp.find(key);
        return (it == pp.end()) ? nullptr : &it->second;
    };
    std::string ax;
    ax += axis;
    const std::string* typeStr = find("scale_type_" + ax);
    if (!typeStr) typeStr = find("scale_type");
    if (typeStr) {
        if      (*typeStr == "viewport") s.type = PassScale::Type::Viewport;
        else if (*typeStr == "absolute") s.type = PassScale::Type::Absolute;
        else                              s.type = PassScale::Type::Source;
    }
    const std::string* scaleStr = find("scale_" + ax);
    if (!scaleStr) scaleStr = find("scale");
    if (scaleStr) {
        try { s.factor = std::stof(*scaleStr); } catch (...) {}
    }
    return s;
}

VkExtent2D applyScale(const PassScale& sx, const PassScale& sy,
                      VkExtent2D prev, VkExtent2D viewport) {
    auto axisExtent = [](const PassScale& s, uint32_t prevDim, uint32_t vpDim) -> uint32_t {
        float v = 1.0f;
        switch (s.type) {
            case PassScale::Type::Source:   v = s.factor * float(prevDim); break;
            case PassScale::Type::Viewport: v = s.factor * float(vpDim);   break;
            case PassScale::Type::Absolute: v = s.factor;                   break;
        }
        if (v < 1.0f)      v = 1.0f;
        if (v > 16384.0f)  v = 16384.0f;
        return static_cast<uint32_t>(v + 0.5f);
    };
    return { axisExtent(sx, prev.width,  viewport.width),
             axisExtent(sy, prev.height, viewport.height) };
}

void transitionImage(VkCommandBuffer cb, VkImage img,
                     VkImageLayout oldL, VkImageLayout newL,
                     VkAccessFlags2 srcAccess, VkAccessFlags2 dstAccess,
                     VkPipelineStageFlags2 srcStage, VkPipelineStageFlags2 dstStage) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask  = srcStage; b.dstStageMask  = dstStage;
    b.srcAccessMask = srcAccess; b.dstAccessMask = dstAccess;
    b.oldLayout     = oldL;     b.newLayout     = newL;
    b.image         = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers    = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

// vec4(width, height, 1/width, 1/height) — the RetroArch *Size convention.
void writeSizeVec4(uint8_t* dst, uint32_t size, VkExtent2D e) {
    const float v[4] = {
        static_cast<float>(e.width),
        static_cast<float>(e.height),
        e.width  ? 1.0f / static_cast<float>(e.width)  : 0.0f,
        e.height ? 1.0f / static_cast<float>(e.height) : 0.0f,
    };
    std::memcpy(dst, v, std::min<uint32_t>(size, sizeof(v)));
}

constexpr float kIdentityMat4[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

} // namespace

bool Preset::isSemanticName(const std::string& name) {
    return name == "MVP" || name == "SourceSize" || name == "OriginalSize"
        || name == "OutputSize" || name == "FinalViewportSize"
        || name == "FrameCount" || name == "FrameDirection"
        || name == "OriginalHistorySize0";
}

Preset::Preset(VulkanContext& ctx, const std::filesystem::path& path,
               VkFormat swapchainFormat)
    : m_path(path), m_ctx(&ctx), m_swapFormat(swapchainFormat) {
    std::ostringstream log;
    bool warn = false;
    ShaderCache cache;
    PresetDef* raw = ShaderGC::CompilePreset(path, log, warn, cache);
    if (!raw) {
        throw std::runtime_error("Preset: ShaderGC::CompilePreset failed for "
                                 + path.string() + "\n" + log.str());
    }
    m_def.reset(raw);

    if (m_def->ShaderDefs.empty()) {
        throw std::runtime_error("Preset: '" + path.string() + "' has 0 shaders");
    }
    if (warn) {
        LOG_WARN("Preset: '%s' compiled with warnings:\n%s",
                 path.string().c_str(), log.str().c_str());
    }
    // Load each .slangp TextureDef into a GPU LutTexture before pipeline
    // construction — pipelines need the views/samplers at descriptor-set
    // creation time.
    auto parseLutSampler = [](const std::map<std::string, std::string>& pp) {
        ShaderPipelineSampler s;
        auto fit = pp.find("linear");
        if (fit != pp.end()) {
            s.linearFilter = (fit->second == "true" || fit->second == "1");
        }
        auto wit = pp.find("wrap_mode");
        if (wit != pp.end()) {
            const std::string& w = wit->second;
            if      (w == "repeat")          s.wrap = ShaderPipelineSampler::Wrap::Repeat;
            else if (w == "mirrored_repeat") s.wrap = ShaderPipelineSampler::Wrap::MirroredRepeat;
            else if (w == "clamp_to_border") s.wrap = ShaderPipelineSampler::Wrap::ClampToBorder;
            else                              s.wrap = ShaderPipelineSampler::Wrap::ClampToEdge;
        }
        return s;
    };
    m_luts.reserve(m_def->TextureDefs.size());
    for (const auto& td : m_def->TextureDefs) {
        try {
            m_luts.push_back(std::make_unique<LutTexture>(
                ctx, td.Data, td.DataLength, parseLutSampler(td.PresetParams)));
        } catch (const std::exception& e) {
            LOG_WARN("Preset: '%s' LUT '%s' failed to load: %s",
                     path.string().c_str(), td.Name.c_str(), e.what());
            m_luts.push_back(nullptr);
        }
    }

    buildPipelines(ctx, swapchainFormat);

    // Aggregate params from EVERY pass for the UI. Each entry's pass
    // index is stored in m_paramPass (parallel vector) so updateUbo()
    // writes back to the right pipeline's UBO at the right offset.
    for (size_t i = 0; i < m_def->ShaderDefs.size(); ++i) {
        for (const auto& p : m_def->ShaderDefs[i].Params) {
            m_params.push_back(p);
            m_paramPass.push_back(static_cast<int>(i));
        }
    }
    applyPresetOverrides();
    updateUbo();
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();
}

void Preset::buildPipelines(VulkanContext& ctx, VkFormat swapFmt) {
    const size_t N = m_def->ShaderDefs.size();
    m_pipelines.reserve(N);
    m_uboSizes.reserve(N);
    m_frameCountMods.reserve(N);

    auto parseSampler = [](const std::map<std::string, std::string>& pp) {
        ShaderPipelineSampler s;
        auto fit = pp.find("filter_linear");
        if (fit != pp.end()) {
            // RetroArch accepts "true"/"false"; some shader packs ship the
            // literal string with mixed case or "1"/"0".
            const std::string& v = fit->second;
            s.linearFilter = (v == "true" || v == "TRUE" || v == "1");
        }
        auto wit = pp.find("wrap_mode");
        if (wit != pp.end()) {
            const std::string& w = wit->second;
            if      (w == "repeat")          s.wrap = ShaderPipelineSampler::Wrap::Repeat;
            else if (w == "mirrored_repeat") s.wrap = ShaderPipelineSampler::Wrap::MirroredRepeat;
            else if (w == "clamp_to_border") s.wrap = ShaderPipelineSampler::Wrap::ClampToBorder;
            else                              s.wrap = ShaderPipelineSampler::Wrap::ClampToEdge;
        }
        return s;
    };

    // Build a quick lookup: slangp-logical-name → LutTexture*.
    std::unordered_map<std::string, LutTexture*> lutByName;
    for (size_t li = 0; li < m_def->TextureDefs.size(); ++li) {
        if (!m_luts[li]) continue;
        const auto& td = m_def->TextureDefs[li];
        auto nit = td.PresetParams.find("name");
        const std::string& key = (nit != td.PresetParams.end()) ? nit->second : td.Name;
        lutByName[key] = m_luts[li].get();
    }

    auto parsePassFormat = [this](const std::map<std::string, std::string>& pp) -> VkFormat {
        auto find = [&](const char* k) -> const std::string* {
            auto it = pp.find(k);
            return (it == pp.end()) ? nullptr : &it->second;
        };
        const std::string* f = find("float_framebuffer");
        if (f && (*f == "true" || *f == "1")) return VK_FORMAT_R16G16B16A16_SFLOAT;
        const std::string* s = find("srgb_framebuffer");
        if (s && (*s == "true" || *s == "1")) return VK_FORMAT_R8G8B8A8_SRGB;
        return m_intermediateFormat;
    };

    auto parseFrameCountMod = [](const std::map<std::string, std::string>& pp) -> uint32_t {
        auto it = pp.find("frame_count_mod");
        if (it == pp.end()) return 0;
        try {
            const long v = std::stol(it->second);
            return (v > 0) ? static_cast<uint32_t>(v) : 0;
        } catch (...) { return 0; }
    };

    m_passOutputFormats.assign(N, VK_FORMAT_UNDEFINED);
    for (size_t i = 0; i < N; ++i) {
        m_passOutputFormats[i] = (i + 1 == N)
            ? swapFmt
            : parsePassFormat(m_def->ShaderDefs[i].PresetParams);
    }

    for (size_t i = 0; i < N; ++i) {
        auto& sd = m_def->ShaderDefs[i];
        const VkFormat passFmt = m_passOutputFormats[i];

        ShaderPipelineSlangConfig cfg;
        cfg.uboSize         = static_cast<uint32_t>(sd.ParamsSize(0));
        cfg.uboBinding      = static_cast<uint32_t>(sd.UboBinding);
        cfg.pushSize        = static_cast<uint32_t>(sd.ParamsSize(-1));
        cfg.usesVertexInput = sd.UsesVertexInput;
        cfg.sampler         = parseSampler(sd.PresetParams);

        if (sd.ParamsSize(1) > 0) {
            LOG_WARN("Preset: '%s' pass %zu uses uniform buffer 1 (%zu bytes); "
                     "only buffer 0 is bound — expect visual artifacts",
                     m_path.string().c_str(), i, sd.ParamsSize(1));
        }

        // Route every reflected sampler to its role: Source, the
        // Original family (incl. graceful degradation for history/feedback
        // samplers we don't keep textures for), or a named LUT.
        for (const auto& smp : sd.Samplers) {
            if (smp.name == "Source") {
                cfg.sourceBinding = smp.binding;
                continue;
            }
            auto lutIt = lutByName.find(smp.name);
            if (lutIt != lutByName.end()) {
                ShaderPipelineLutBinding lb;
                lb.binding = static_cast<uint32_t>(smp.binding);
                lb.view    = lutIt->second->view();
                lb.sampler = lutIt->second->sampler();
                cfg.luts.push_back(lb);
                continue;
            }
            if (smp.name != "Original" && smp.name != "OriginalHistory0") {
                LOG_WARN("Preset: '%s' pass %zu samples '%s' (history/feedback "
                         "textures are not supported yet) — binding the original "
                         "input instead",
                         m_path.string().c_str(), i, smp.name.c_str());
            }
            cfg.originalBindings.push_back(static_cast<uint32_t>(smp.binding));
        }

        m_pipelines.push_back(std::make_unique<ShaderPipeline>(
            ctx, sd.VertexByteCode,   sd.VertexLength,
                 sd.FragmentByteCode, sd.FragmentLength,
            passFmt, std::move(cfg)));
        m_uboSizes.push_back(static_cast<uint32_t>(sd.ParamsSize(0)));
        m_frameCountMods.push_back(parseFrameCountMod(sd.PresetParams));
    }
}

void Preset::applyPresetOverrides() {
    // .slangp files can pin parameters: `SCANLINE_STRENGTH = 0.5`. ShaderGC
    // parses these into PresetDef::Overrides; apply them as the new current
    // AND default values (RetroArch treats overrides as the preset's
    // baseline, and "reset to defaults" returns to them).
    for (const auto& o : m_def->Overrides) {
        for (auto& p : m_params) {
            if (p.name == o.name && isUserParam(p)) {
                p.currentValue = o.value;
                p.defaultValue = o.value;
            }
        }
    }
}

void Preset::resetParamsToDefaults() {
    // Values reach the GPU on the next advanceFrame()/updateUbo() — the
    // windowed loop runs that after waiting on in-flight frames, so writing
    // the mapped buffers here (mid-UI-draw) would race pending GPU reads.
    for (auto& p : m_params) p.currentValue = p.defaultValue;
}

VkExtent2D Preset::passInputExtent(int passIdx) const {
    const size_t i = static_cast<size_t>(passIdx);
    if (i == 0 || m_intermediates.empty() || i - 1 >= m_intermediates.size()
        || !m_intermediates[i - 1]) {
        return { m_srcWidth, m_srcHeight };
    }
    return m_intermediates[i - 1]->extent();
}

VkExtent2D Preset::passOutputExtent(int passIdx) const {
    const size_t i = static_cast<size_t>(passIdx);
    if (i + 1 < m_pipelines.size() && i < m_intermediates.size() && m_intermediates[i]) {
        return m_intermediates[i]->extent();
    }
    const uint32_t w = m_vpWidth  ? m_vpWidth  : m_srcWidth;
    const uint32_t h = m_vpHeight ? m_vpHeight : m_srcHeight;
    return { w, h };
}

void Preset::writeParamValue(uint8_t* dst, const ShaderParam& p, int passIdx) const {
    const uint32_t size = static_cast<uint32_t>(p.size);

    if (p.name == "MVP") {
        std::memcpy(dst, kIdentityMat4, std::min<uint32_t>(size, sizeof(kIdentityMat4)));
    } else if (p.name == "SourceSize") {
        writeSizeVec4(dst, size, passInputExtent(passIdx));
    } else if (p.name == "OriginalSize" || p.name == "OriginalHistorySize0") {
        writeSizeVec4(dst, size, { m_srcWidth, m_srcHeight });
    } else if (p.name == "OutputSize") {
        writeSizeVec4(dst, size, passOutputExtent(passIdx));
    } else if (p.name == "FinalViewportSize") {
        const uint32_t w = m_vpWidth  ? m_vpWidth  : m_srcWidth;
        const uint32_t h = m_vpHeight ? m_vpHeight : m_srcHeight;
        writeSizeVec4(dst, size, { w, h });
    } else if (p.name == "FrameCount") {
        const uint32_t mod = (passIdx >= 0 &&
                              static_cast<size_t>(passIdx) < m_frameCountMods.size())
                                 ? m_frameCountMods[passIdx] : 0;
        const uint32_t fc = mod ? (m_frameCount % mod) : m_frameCount;
        std::memcpy(dst, &fc, std::min<uint32_t>(size, sizeof(fc)));
    } else if (p.name == "FrameDirection") {
        const int32_t dir = 1;
        std::memcpy(dst, &dir, std::min<uint32_t>(size, sizeof(dir)));
    } else if (isUserParam(p)) {
        std::memcpy(dst, &p.currentValue, sizeof(float));
    }
    // Unknown non-user members (e.g. stock.slang's `_unused`) stay zero.
}

void Preset::updateUbo() {
    // Walk the aggregated params; m_paramPass tells us which pipeline's
    // UBO / push-constant staging to write each entry into.
    for (size_t i = 0; i < m_params.size(); ++i) {
        const int passIdx = m_paramPass[i];
        if (passIdx < 0 || passIdx >= static_cast<int>(m_pipelines.size())) continue;
        ShaderPipeline& pipe = *m_pipelines[passIdx];
        const auto& p = m_params[i];

        uint8_t* dest     = nullptr;
        uint32_t destSize = 0;
        if (p.buffer == 0) {
            dest     = static_cast<uint8_t*>(pipe.mappedUbo());
            destSize = pipe.uboSizeBytes();
        } else if (p.buffer == -1) {
            dest     = static_cast<uint8_t*>(pipe.mappedPush());
            destSize = pipe.pushSizeBytes();
        } else {
            continue; // unsupported buffer index (warned at build time)
        }
        if (!dest || destSize == 0) continue;
        if (p.offset < 0 || p.size < 0 ||
            static_cast<uint32_t>(p.offset) + static_cast<uint32_t>(p.size) > destSize) {
            continue; // defensive: never write out of bounds
        }
        writeParamValue(dest + p.offset, p, passIdx);
    }
}

void Preset::advanceFrame() {
    ++m_frameCount;
    updateUbo();
}

void Preset::ensureSourceSize(uint32_t srcWidth, uint32_t srcHeight,
                              uint32_t viewportWidth, uint32_t viewportHeight) {
    if (!isMultiPass() || srcWidth == 0 || srcHeight == 0) {
        m_srcWidth  = srcWidth;
        m_srcHeight = srcHeight;
        m_vpWidth   = viewportWidth;
        m_vpHeight  = viewportHeight;
        updateUbo();
        return;
    }
    // Viewport-relative scales require a viewport; fall back to source
    // for callers (e.g. tests) that don't have a swapchain.
    if (viewportWidth  == 0) viewportWidth  = srcWidth;
    if (viewportHeight == 0) viewportHeight = srcHeight;

    if (srcWidth == m_srcWidth && srcHeight == m_srcHeight
        && viewportWidth  == m_vpWidth && viewportHeight == m_vpHeight
        && !m_intermediates.empty()) return;

    vkDeviceWaitIdle(m_ctx->device());
    m_intermediates.clear();
    const size_t passCount = m_pipelines.size();
    m_intermediates.reserve(passCount - 1);

    VkExtent2D prev{srcWidth, srcHeight};
    const VkExtent2D viewport{viewportWidth, viewportHeight};
    for (size_t i = 0; i + 1 < passCount; ++i) {
        const auto& sd = m_def->ShaderDefs[i];
        const PassScale sx = parseScale(sd.PresetParams, 'x');
        const PassScale sy = parseScale(sd.PresetParams, 'y');
        const VkExtent2D out = applyScale(sx, sy, prev, viewport);
        const VkFormat   fmt = (i < m_passOutputFormats.size())
                                   ? m_passOutputFormats[i]
                                   : m_intermediateFormat;
        m_intermediates.push_back(std::make_unique<OffscreenTarget>(
            *m_ctx, out.width, out.height, fmt));
        prev = out;
    }
    m_srcWidth  = srcWidth;
    m_srcHeight = srcHeight;
    m_vpWidth   = viewportWidth;
    m_vpHeight  = viewportHeight;
    // Sizes changed → refresh SourceSize/OutputSize semantics in every pass.
    updateUbo();
}

void Preset::recordIntermediatePasses(VkCommandBuffer cb,
                                      VkImageView sourceView,
                                      VkExtent2D  sourceExtent) {
    m_originalView = sourceView;

    if (!isMultiPass() || m_intermediates.empty()) {
        m_finalInputView   = sourceView;
        m_finalInputExtent = sourceExtent;
        return;
    }

    VkImageView readView   = sourceView;
    VkExtent2D  readExtent = sourceExtent;

    const size_t passes = m_pipelines.size();
    for (size_t i = 0; i + 1 < passes; ++i) {
        OffscreenTarget& tgt = *m_intermediates[i];

        transitionImage(cb, tgt.image(),
                        tgt.currentLayout(),
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView   = tgt.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{ 0.0f, 0.0f, 0.0f, 1.0f }};

        VkRenderingInfo rinfo{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rinfo.renderArea           = { {0,0}, tgt.extent() };
        rinfo.layerCount           = 1;
        rinfo.colorAttachmentCount = 1;
        rinfo.pColorAttachments    = &color;

        vkCmdBeginRendering(cb, &rinfo);
        m_pipelines[i]->bindAndDrawWithImageView(cb, readView, tgt.extent(), m_originalView);
        vkCmdEndRendering(cb);

        transitionImage(cb, tgt.image(),
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                        VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                        VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
        tgt.setLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        readView   = tgt.view();
        readExtent = tgt.extent();
    }

    m_finalInputView   = readView;
    m_finalInputExtent = readExtent;
}

void Preset::drawFinalPass(VkCommandBuffer cb, VkExtent2D viewport) {
    finalPipeline().bindAndDrawWithImageView(cb, m_finalInputView, viewport, m_originalView);
}
