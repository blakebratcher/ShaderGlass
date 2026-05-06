#include "ShaderPipeline.h"
#include "VulkanContext.h"
#include "Texture.h"
#include "../util/VkCheck.h"

static VkShaderModule makeModule(VkDevice dev, const void* code, size_t size) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = size;
    ci.pCode    = static_cast<const uint32_t*>(code);
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(dev, &ci, nullptr, &m));
    return m;
}

ShaderPipeline::ShaderPipeline(VulkanContext& ctx,
                               const void* vertSpv, size_t vertSize,
                               const void* fragSpv, size_t fragSize,
                               VkFormat colorFormat)
    : m_ctx(ctx) {

    VkSamplerCreateInfo samp{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    samp.magFilter    = VK_FILTER_LINEAR;
    samp.minFilter    = VK_FILTER_LINEAR;
    samp.addressModeU = samp.addressModeV = samp.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(ctx.device(), &samp, nullptr, &m_sampler));

    VkDescriptorSetLayoutBinding b{};
    b.binding         = 0;
    b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1;
    b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dsli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsli.bindingCount = 1; dsli.pBindings = &b;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dsli, nullptr, &m_dsl));

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1; pli.pSetLayouts = &m_dsl;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &pli, nullptr, &m_pipelineLayout));

    VkDescriptorPoolSize ps{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1; dpi.poolSizeCount = 1; dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &m_dsp));

    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = m_dsp; dsai.descriptorSetCount = 1; dsai.pSetLayouts = &m_dsl;
    VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &dsai, &m_ds));

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
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

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
    gpi.pDynamicState       = &ds;
    gpi.layout              = m_pipelineLayout;
    VkResult pipelineResult =
        vkCreateGraphicsPipelines(ctx.device(), VK_NULL_HANDLE, 1, &gpi, nullptr, &m_pipeline);
    vkDestroyShaderModule(ctx.device(), vmod, nullptr);
    vkDestroyShaderModule(ctx.device(), fmod, nullptr);
    VK_CHECK(pipelineResult);
}

ShaderPipeline::~ShaderPipeline() {
    vkDeviceWaitIdle(m_ctx.device());
    if (m_pipeline)        vkDestroyPipeline           (m_ctx.device(), m_pipeline,       nullptr);
    if (m_pipelineLayout)  vkDestroyPipelineLayout     (m_ctx.device(), m_pipelineLayout, nullptr);
    if (m_dsp)             vkDestroyDescriptorPool     (m_ctx.device(), m_dsp,            nullptr);
    if (m_dsl)             vkDestroyDescriptorSetLayout(m_ctx.device(), m_dsl,            nullptr);
    if (m_sampler)         vkDestroySampler            (m_ctx.device(), m_sampler,        nullptr);
}

void ShaderPipeline::bindAndDraw(VkCommandBuffer cb, const Texture& src, VkExtent2D viewport) {
    VkDescriptorImageInfo ii{};
    ii.sampler     = m_sampler;
    ii.imageView   = src.view();
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = m_ds; w.dstBinding = 0;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
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
