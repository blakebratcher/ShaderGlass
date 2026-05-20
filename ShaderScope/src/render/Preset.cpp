#include "Preset.h"
#include "VulkanContext.h"
#include "ShaderGC.h"
#include "ShaderCache.h"
#include "PresetDef.h"
#include "util/Logging.h"
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace {
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
} // namespace

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
    if (!m_def->TextureDefs.empty()) {
        LOG_WARN("Preset: '%s' declares %zu lookup texture(s); LUTs are not "
                 "yet implemented on Linux — expect visual artifacts.",
                 path.string().c_str(), m_def->TextureDefs.size());
    }

    buildPipelines(ctx, swapchainFormat);

    // UI surfaces params from the LAST pass.
    auto& finalDef = m_def->ShaderDefs.back();
    m_params = finalDef.Params;
    updateUbo();
}

Preset::~Preset() {
    if (m_def) m_def->MakeDynamic();
}

void Preset::buildPipelines(VulkanContext& ctx, VkFormat swapFmt) {
    const size_t N = m_def->ShaderDefs.size();
    m_pipelines.reserve(N);
    m_uboSizes.reserve(N);

    for (size_t i = 0; i < N; ++i) {
        auto& sd = m_def->ShaderDefs[i];
        const VkFormat passFmt = (i + 1 == N) ? swapFmt : m_intermediateFormat;
        const uint32_t uboSize = static_cast<uint32_t>(sd.ParamsSize(0));
        if (sd.ParamsSize(1) > 0) {
            LOG_WARN("Preset: '%s' pass %zu uses uniform buffer 1 (%zu bytes); "
                     "only buffer 0 is bound — expect visual artifacts",
                     m_path.string().c_str(), i, sd.ParamsSize(1));
        }
        if (uboSize == 0) {
            m_pipelines.push_back(std::make_unique<ShaderPipeline>(
                ctx, sd.VertexByteCode,   sd.VertexLength,
                     sd.FragmentByteCode, sd.FragmentLength,
                passFmt));
        } else {
            m_pipelines.push_back(std::make_unique<ShaderPipeline>(
                ctx, sd.VertexByteCode,   sd.VertexLength,
                     sd.FragmentByteCode, sd.FragmentLength,
                passFmt, uboSize, ShaderPipeline::WithParamsTag{}));
        }
        m_uboSizes.push_back(uboSize);
    }
}

void Preset::resetParamsToDefaults() {
    for (auto& p : m_params) p.currentValue = p.defaultValue;
    updateUbo();
}

void Preset::updateUbo() {
    const size_t N = m_pipelines.size();
    for (size_t i = 0; i < N; ++i) {
        void* ubo = m_pipelines[i]->mappedUbo();
        if (!ubo || m_uboSizes[i] == 0) continue;
        const auto& srcParams = (i + 1 == N) ? m_params : m_def->ShaderDefs[i].Params;
        for (const auto& p : srcParams) {
            if (p.buffer != 0) continue;
            std::memcpy(static_cast<uint8_t*>(ubo) + p.offset, &p.currentValue,
                        sizeof(float));
        }
    }
}

void Preset::ensureSourceSize(uint32_t srcWidth, uint32_t srcHeight) {
    if (!isMultiPass() || srcWidth == 0 || srcHeight == 0) {
        m_srcWidth  = srcWidth;
        m_srcHeight = srcHeight;
        return;
    }
    if (srcWidth == m_srcWidth && srcHeight == m_srcHeight
        && !m_intermediates.empty()) return;

    vkDeviceWaitIdle(m_ctx->device());
    m_intermediates.clear();
    const size_t passCount = m_pipelines.size();
    m_intermediates.reserve(passCount - 1);
    for (size_t i = 0; i + 1 < passCount; ++i) {
        m_intermediates.push_back(std::make_unique<OffscreenTarget>(
            *m_ctx, srcWidth, srcHeight, m_intermediateFormat));
    }
    m_srcWidth  = srcWidth;
    m_srcHeight = srcHeight;
}

void Preset::recordIntermediatePasses(VkCommandBuffer cb,
                                      VkImageView sourceView,
                                      VkExtent2D  sourceExtent) {
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
        m_pipelines[i]->bindAndDrawWithImageView(cb, readView, tgt.extent());
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
