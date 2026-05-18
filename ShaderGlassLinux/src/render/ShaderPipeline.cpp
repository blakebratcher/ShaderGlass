#include "ShaderPipeline.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "../util/VkCheck.h"
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
                               VkFormat colorFormat)
    : m_ctx(ctx) {
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat, 0);
}

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat,
                               uint32_t uboSize,
                               WithParamsTag)
    : m_ctx(ctx) {
    createPipeline(ctx, vertSpv, vertSize, fragSpv, fragSize, colorFormat, uboSize);
}

// ── destructor ────────────────────────────────────────────────────────────────

ShaderPipeline::~ShaderPipeline() {
    vkDeviceWaitIdle(m_ctx.device());
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
                                    VkFormat colorFormat,
                                    uint32_t uboSize) {
    // ── Sampler ──────────────────────────────────────────────────────────────
    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter    = VK_FILTER_LINEAR;
    samp.minFilter    = VK_FILTER_LINEAR;
    samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &samp, nullptr, &m_sampler));

    // ── Descriptor set layout ────────────────────────────────────────────────
    // Passthrough (uboSize == 0): one binding — combined image sampler at 0.
    // Slang (uboSize > 0):        two bindings — UBO at 0, sampler at 2.
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    if (uboSize > 0) {
        VkDescriptorSetLayoutBinding bindings[2]{};
        bindings[0].binding         = 0;
        bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding         = 2;
        bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        dsli.bindingCount = 2; dsli.pBindings = bindings;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsli, nullptr, &m_dsl));
    } else {
        VkDescriptorSetLayoutBinding b{};
        b.binding         = 0;
        b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b.descriptorCount = 1;
        b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        dsli.bindingCount = 1; dsli.pBindings = &b;
        VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsli, nullptr, &m_dsl));
    }

    // ── Pipeline layout ──────────────────────────────────────────────────────
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &m_dsl;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &m_pipelineLayout));

    // ── Descriptor pool ──────────────────────────────────────────────────────
    if (uboSize > 0) {
        VkDescriptorPoolSize ps[2]{};
        ps[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,          1 };
        ps[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,  1 };
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.maxSets = 1; dpi.poolSizeCount = 2; dpi.pPoolSizes = ps;
        VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &m_dsp));
    } else {
        VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
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

        // Write UBO descriptor at binding 0
        VkDescriptorBufferInfo bi{};
        bi.buffer = m_uboBuffer; bi.offset = 0; bi.range = uboSize;
        VkWriteDescriptorSet wUbo{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wUbo.dstSet          = m_ds;
        wUbo.dstBinding      = 0;
        wUbo.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        wUbo.descriptorCount = 1;
        wUbo.pBufferInfo     = &bi;
        vkUpdateDescriptorSets(ctx.device(), 1, &wUbo, 0, nullptr);
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

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

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

void ShaderPipeline::bindAndDrawWithImageView(VkCommandBuffer cb, VkImageView view, VkExtent2D viewport) {
    VkDescriptorImageInfo ii{};
    ii.sampler     = m_sampler;
    ii.imageView   = view;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = m_ds;
    w.dstBinding      = (m_uboSize > 0) ? 2u : 0u;  // slang: sampler@2, passthrough: sampler@0
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1; w.pImageInfo = &ii;
    vkUpdateDescriptorSets(m_ctx.device(), 1, &w, 0, nullptr);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
        m_pipelineLayout, 0, 1, &m_ds, 0, nullptr);

    VkViewport vp{ 0, 0, (float)viewport.width, (float)viewport.height, 0.0f, 1.0f };
    VkRect2D   sc{ {0,0}, viewport };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor (cb, 0, 1, &sc);
    vkCmdDraw(cb, 3, 1, 0, 0);
}

void ShaderPipeline::bindAndDraw(VkCommandBuffer cb, const Texture& src, VkExtent2D viewport) {
    bindAndDrawWithImageView(cb, src.view(), viewport);
}
