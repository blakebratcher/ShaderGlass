#include "ShaderPipeline.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "../util/VkCheck.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

static VkShaderModule makeModule(VkDevice dev, const void* code, size_t size) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = static_cast<const uint32_t*>(code);
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

// ── public constructors ───────────────────────────────────────────────────────

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat,
                               ShaderPipelineSampler sampler)
    : m_ctx(ctx) {
    m_config.sampler = sampler;
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat);
}

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat,
                               ShaderPipelineSlangConfig config)
    : m_ctx(ctx), m_config(std::move(config)) {
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat);
}

// ── destructor ────────────────────────────────────────────────────────────────

ShaderPipeline::~ShaderPipeline() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_vboMapped)  vkUnmapMemory   (m_ctx.device(), m_vboMemory);
    if (m_vboMemory)  vkFreeMemory    (m_ctx.device(), m_vboMemory, nullptr);
    if (m_vbo)        vkDestroyBuffer (m_ctx.device(), m_vbo, nullptr);
    if (m_uboMapped)  vkUnmapMemory   (m_ctx.device(), m_uboMemory);
    if (m_uboMemory)  vkFreeMemory    (m_ctx.device(), m_uboMemory, nullptr);
    if (m_uboBuffer)  vkDestroyBuffer (m_ctx.device(), m_uboBuffer, nullptr);
    if (m_pipeline)        vkDestroyPipeline           (m_ctx.device(), m_pipeline,       nullptr);
    if (m_pipelineLayout)  vkDestroyPipelineLayout     (m_ctx.device(), m_pipelineLayout, nullptr);
    if (m_dsp)             vkDestroyDescriptorPool     (m_ctx.device(), m_dsp,            nullptr);
    if (m_dsl)             vkDestroyDescriptorSetLayout(m_ctx.device(), m_dsl,            nullptr);
    if (m_sampler)         vkDestroySampler            (m_ctx.device(), m_sampler,        nullptr);
}

// ── private shared implementation ────────────────────────────────────────────

void ShaderPipeline::createPipeline(VulkanContext& ctx,
                                    const void* vertSpv, size_t vertSize,
                                    const void* fragSpv, size_t fragSize,
                                    VkFormat colorFormat) {
    const uint32_t uboSize  = m_config.uboSize;
    const uint32_t pushSize = m_config.pushSize;

    // Resolved Source binding: reflected if available, else the historic
    // defaults (binding 2 alongside a UBO, binding 0 for passthrough).
    m_sourceBinding = (m_config.sourceBinding >= 0)
        ? static_cast<uint32_t>(m_config.sourceBinding)
        : (uboSize > 0 ? 2u : 0u);

    // ── Sampler ──────────────────────────────────────────────────────────────
    // filter_linear / wrap_mode from the .slangp settle here.
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const VkFilter filter = m_config.sampler.linearFilter
                                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    samp.magFilter = filter;
    samp.minFilter = filter;
    VkSamplerAddressMode wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    switch (m_config.sampler.wrap) {
        case ShaderPipelineSampler::Wrap::Repeat:
            wrap = VK_SAMPLER_ADDRESS_MODE_REPEAT;          break;
        case ShaderPipelineSampler::Wrap::MirroredRepeat:
            wrap = VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT; break;
        case ShaderPipelineSampler::Wrap::ClampToBorder:
            wrap = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; break;
        case ShaderPipelineSampler::Wrap::ClampToEdge:
        default: break;
    }
    samp.addressModeU = samp.addressModeV = samp.addressModeW = wrap;
    VK_CHECK(vkCreateSampler(ctx.device(), &samp, nullptr, &m_sampler));

    // ── Descriptor set layout ────────────────────────────────────────────────
    // Layout is driven entirely by the reflection-derived config:
    //   - UBO at config.uboBinding when the shader declares one
    //   - Source sampler at the reflected binding
    //   - "Original"-family samplers at their reflected bindings
    //   - one COMBINED_IMAGE_SAMPLER per LUT at its reflected binding
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(2 + m_config.originalBindings.size() + m_config.luts.size());
    if (uboSize > 0) {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = m_config.uboBinding;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    }
    auto addSamplerBinding = [&bindings](uint32_t slot) {
        for (const auto& existing : bindings) {
            if (existing.binding == slot) return;  // already declared
        }
        VkDescriptorSetLayoutBinding b{};
        b.binding         = slot;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(b);
    };
    addSamplerBinding(m_sourceBinding);
    for (uint32_t ob : m_config.originalBindings) addSamplerBinding(ob);
    for (const auto& lut : m_config.luts)         addSamplerBinding(lut.binding);

    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = static_cast<uint32_t>(bindings.size());
    dsli.pBindings    = bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsli, nullptr, &m_dsl));

    // ── Pipeline layout ──────────────────────────────────────────────────────
    // Push constants:
    //   - Slang shaders with their own push_constant block: VERTEX|FRAGMENT
    //     range sized to the reflected block; Preset fills mappedPush().
    //   - Otherwise: legacy vec4 uvTransform (16 bytes) in fragment stage,
    //     consumed by the builtin passthrough shader for crop.
    VkPushConstantRange pcRange{};
    if (pushSize > 0) {
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset     = 0;
        pcRange.size       = pushSize;
        m_pushStaging.assign(pushSize, 0);
    } else {
        pcRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset     = 0;
        pcRange.size       = sizeof(float) * 4;  // vec4
    }

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount         = 1; pli.pSetLayouts = &m_dsl;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcRange;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &m_pipelineLayout));

    // ── Descriptor pool ──────────────────────────────────────────────────────
    const uint32_t samplerCount =
        static_cast<uint32_t>(1 + m_config.originalBindings.size() + m_config.luts.size());
    if (uboSize > 0) {
        VkDescriptorPoolSize ps[2]{};
        ps[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1 };
        ps[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount };
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &m_dsp));
    } else {
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount };
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &m_dsp));
    }

    // ── Descriptor set allocation ────────────────────────────────────────────
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = m_dsp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_dsl;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsai, &m_ds));

    // ── UBO allocation (slang path only) ─────────────────────────────────────
    if (uboSize > 0) {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = uboSize;
        bci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(ctx.device(), &bci, nullptr, &m_uboBuffer) != VK_SUCCESS)
            throw std::runtime_error("ShaderPipeline: vkCreateBuffer (UBO) failed");

        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device(), m_uboBuffer, &mr);

        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = ctx.findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(ctx.device(), &mai, nullptr, &m_uboMemory) != VK_SUCCESS)
            throw std::runtime_error("ShaderPipeline: vkAllocateMemory (UBO) failed");

        VK_CHECK(vkBindBufferMemory(ctx.device(), m_uboBuffer, m_uboMemory, 0));
        VK_CHECK(vkMapMemory(ctx.device(), m_uboMemory, 0, uboSize, 0, &m_uboMapped));
        m_uboSize = uboSize;
        std::memset(m_uboMapped, 0, uboSize);

        // Write UBO descriptor at its binding
        VkDescriptorBufferInfo bi{};
        bi.buffer = m_uboBuffer; bi.offset = 0; bi.range = uboSize;
        VkWriteDescriptorSet wUbo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wUbo.dstSet          = m_ds;
        wUbo.dstBinding      = m_config.uboBinding;
        wUbo.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wUbo.descriptorCount = 1;
        wUbo.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(ctx.device(), 1, &wUbo, 0, nullptr);
    }

    // LUTs are static for the preset's lifetime — bind once here.
    if (!m_config.luts.empty()) {
        std::vector<VkDescriptorImageInfo> ii(m_config.luts.size());
        std::vector<VkWriteDescriptorSet>  writes(m_config.luts.size(),
            VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET});
        for (size_t i = 0; i < m_config.luts.size(); ++i) {
            ii[i].imageView   = m_config.luts[i].view;
            ii[i].sampler     = m_config.luts[i].sampler;
            ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            writes[i].dstSet          = m_ds;
            writes[i].dstBinding      = m_config.luts[i].binding;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo      = &ii[i];
        }
        vkUpdateDescriptorSets(ctx.device(),
            static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // ── Fullscreen-quad VBO (vertex-input shaders only) ─────────────────────
    // 4 vertices × (vec2 position + vec2 texcoord), drawn as a triangle
    // strip. Position reads as vec4 in the shader (missing zw default to
    // 0,1 per the Vulkan attribute-expansion rules); MVP is identity so the
    // quad spans the full viewport exactly like the gl_VertexIndex triangle.
    if (m_config.usesVertexInput) {
        constexpr VkDeviceSize kVboSize = 4 * 4 * sizeof(float);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = kVboSize;
        bci.usage       = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(ctx.device(), &bci, nullptr, &m_vbo) != VK_SUCCESS)
            throw std::runtime_error("ShaderPipeline: vkCreateBuffer (quad VBO) failed");

        VkMemoryRequirements mr{};
        vkGetBufferMemoryRequirements(ctx.device(), m_vbo, &mr);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = mr.size;
        mai.memoryTypeIndex = ctx.findMemoryType(mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (vkAllocateMemory(ctx.device(), &mai, nullptr, &m_vboMemory) != VK_SUCCESS)
            throw std::runtime_error("ShaderPipeline: vkAllocateMemory (quad VBO) failed");
        VK_CHECK(vkBindBufferMemory(ctx.device(), m_vbo, m_vboMemory, 0));
        VK_CHECK(vkMapMemory(ctx.device(), m_vboMemory, 0, kVboSize, 0, &m_vboMapped));
        writeQuadVbo();
    }

    // ── Graphics pipeline ─────────────────────────────────────────────────────
    VkShaderModule vmod = makeModule(ctx.device(), vertSpv, vertSize);
    VkShaderModule fmod = makeModule(ctx.device(), fragSpv, fragSize);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vmod; stages[0].pName = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fmod; stages[1].pName = "main";

    // Vertex input + assembly: quad strip for vertex-input shaders,
    // fullscreen triangle (no attributes) otherwise.
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkVertexInputBindingDescription   vibd{};
    VkVertexInputAttributeDescription attrs[2]{};
    if (m_config.usesVertexInput) {
        vibd.binding   = 0;
        vibd.stride    = 4 * sizeof(float);
        vibd.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };                  // Position (xy; zw → 0,1)
        attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float) };  // TexCoord
        vi.vertexBindingDescriptionCount   = 1;
        vi.pVertexBindingDescriptions      = &vibd;
        vi.vertexAttributeDescriptionCount = 2;
        vi.pVertexAttributeDescriptions    = attrs;
    }
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = m_config.usesVertexInput
        ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = VK_CULL_MODE_NONE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynState.dynamicStateCount = 2; dynState.pDynamicStates = dyn;

    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount    = 1;
    prci.pColorAttachmentFormats = &colorFormat;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.pNext               = &prci;
    gpi.stageCount          = 2; gpi.pStages = stages;
    gpi.pVertexInputState   = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dynState;
    gpi.layout              = m_pipelineLayout;
    VkResult pipelineResult =
        vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);
    VK_CHECK(pipelineResult);
}

void ShaderPipeline::writeQuadVbo() noexcept {
    if (!m_vboMapped) return;
    const float u0 = m_uvTransform[0], v0 = m_uvTransform[1];
    const float u1 = m_uvTransform[2], v1 = m_uvTransform[3];
    // Triangle strip; Vulkan NDC has +Y down, texcoord (0,0) is the first
    // texel row, so NDC top-left (-1,-1) samples (u0,v0).
    const float quad[16] = {
        // x      y      u   v
        -1.0f, -1.0f,  u0, v0,
        -1.0f, +1.0f,  u0, v1,
        +1.0f, -1.0f,  u1, v0,
        +1.0f, +1.0f,  u1, v1,
    };
    std::memcpy(m_vboMapped, quad, sizeof(quad));
}

void ShaderPipeline::bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view,
                                              VkExtent2D viewport, VkImageView originalView) {
    // Per-draw image descriptors: Source at its reflected binding, plus the
    // original-input view at every "Original"-family binding.
    std::vector<VkDescriptorImageInfo> imageInfos;
    std::vector<VkWriteDescriptorSet>  writes;
    imageInfos.reserve(1 + m_config.originalBindings.size());
    writes.reserve(1 + m_config.originalBindings.size());

    auto addImageWrite = [&](uint32_t binding, VkImageView v) {
        VkDescriptorImageInfo ii{};
        ii.sampler     = m_sampler;
        ii.imageView   = v;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfos.push_back(ii);
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet          = m_ds;
        w.dstBinding      = binding;
        w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.descriptorCount = 1;
        writes.push_back(w);
    };

    addImageWrite(m_sourceBinding, view);
    const VkImageView origView = (originalView != VK_NULL_HANDLE) ? originalView : view;
    for (uint32_t ob : m_config.originalBindings) {
        if (ob == m_sourceBinding) continue;
        addImageWrite(ob, origView);
    }
    // pImageInfo must be assigned after the vectors stop reallocating.
    for (size_t i = 0; i < writes.size(); ++i) writes[i].pImageInfo = &imageInfos[i];
    vkUpdateDescriptorSets(m_ctx.device(),
        static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 0, 1, &m_ds, 0, nullptr);

    // Push constants: the shader's own block (filled by Preset semantics +
    // params), or the legacy uvTransform vec4 for the builtin passthrough.
    if (!m_pushStaging.empty()) {
        vkCmdPushConstants(cb, m_pipelineLayout,
            VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            0, static_cast<uint32_t>(m_pushStaging.size()), m_pushStaging.data());
    } else {
        vkCmdPushConstants(cb, m_pipelineLayout,
            VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(m_uvTransform), m_uvTransform);
    }

    VkViewport vp{ 0, 0, (float)viewport.width, (float)viewport.height, 0.0f, 1.0f };
    VkRect2D   sc{ {0,0}, viewport };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor (cb, 0, 1, &sc);

    if (m_config.usesVertexInput) {
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &m_vbo, &offset);
        vkCmdDraw(cb, 4, 1, 0, 0);
    } else {
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
}

void ShaderPipeline::setUvTransform(float u0, float v0, float u1, float v1) noexcept {
    m_uvTransform[0] = u0;
    m_uvTransform[1] = v0;
    m_uvTransform[2] = u1;
    m_uvTransform[3] = v1;
    // Vertex-input shaders consume the crop through the quad's texcoords.
    writeQuadVbo();
}

void ShaderPipeline::bindAndDraw(VkCommandBuffer cb, const Texture& src, VkExtent2D viewport) {
    bindAndDrawWithImageView(cb, src.view(), viewport);
}
