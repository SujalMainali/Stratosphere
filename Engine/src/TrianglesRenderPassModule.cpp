#include "Engine/TrianglesRenderPassModule.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "Engine/PerformanceMonitor.h"
#include "utils/BufferUtils.h"
#include <stdexcept>
#include <cstring>

namespace Engine
{

    TrianglesRenderPassModule::~TrianglesRenderPassModule()
    {
        destroyResources();
    }

    void TrianglesRenderPassModule::setVertexBinding(const VertexBinding &binding)
    {
        m_binding = binding;
    }

    void TrianglesRenderPassModule::onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs)
    {
        (void)fbs; // not needed here
        m_device = ctx.GetDevice();
        // Initialize extent from current swapchain so dynamic viewport/scissor have valid size
        if (ctx.GetSwapChain())
            m_extent = ctx.GetSwapChain()->GetExtent();

        // Ensure we always have an instance buffer bound (even for a single draw)
        // Instance data layout: { vec2 offset; vec3 color; }
        const float defaultInstance[5] = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        VkResult instRes = CreateOrUpdateVertexBuffer(
            ctx.GetDevice(),
            ctx.GetPhysicalDevice(),
            defaultInstance,
            sizeof(defaultInstance),
            m_defaultInstanceVB);
        if (instRes != VK_SUCCESS)
        {
            throw std::runtime_error("Triangles: failed to create default instance buffer");
        }

        // Assume swapchain extent comes via context; if renderer exposes it, you can pass.
        // We'll set viewport/scissor dynamically in record.
        createPipeline(ctx, pass);
    }

    void TrianglesRenderPassModule::onResize(VulkanContext &ctx, VkExtent2D newExtent)
    {
        // With dynamic viewport/scissor, pipeline can remain. If you changed formats/subpasses, recreate.
        m_extent = newExtent;
    }

    void TrianglesRenderPassModule::record(FrameContext &frameCtx, VkCommandBuffer cmd)
    {
        (void)frameCtx;
        // Bind pipeline
        m_pipeline.bind(cmd);
        // Push offset: include all stages declared in the pipeline layout's push constant range
        vkCmdPushConstants(cmd, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 2, m_offset);

        // Dynamic viewport & scissor covering the current framebuffer extent
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(m_extent.width);
        viewport.height = static_cast<float>(m_extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = m_extent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Bind vertex + instance buffers
        if (m_binding.vertexBuffer == VK_NULL_HANDLE || m_binding.vertexCount == 0)
            return;

        VkBuffer buffers[2] = {m_binding.vertexBuffer,
                               (m_instances.instanceBuffer != VK_NULL_HANDLE ? m_instances.instanceBuffer : m_defaultInstanceVB.buffer)};
        VkDeviceSize offsets[2] = {m_binding.offset,
                                   (m_instances.instanceBuffer != VK_NULL_HANDLE ? m_instances.offset : 0)};
        vkCmdBindVertexBuffers(cmd, 0, 2, buffers, offsets);

        const uint32_t instanceCount = (m_instances.instanceCount > 0 ? m_instances.instanceCount : 1);
        vkCmdDraw(cmd, m_binding.vertexCount, instanceCount, 0, 0);
        DrawCallCounter::increment();
    }

    void TrianglesRenderPassModule::onDestroy(VulkanContext &ctx)
    {
        destroyResources();
    }

    void TrianglesRenderPassModule::destroyResources()
    {
        if (m_device != VK_NULL_HANDLE)
        {
            m_pipeline.destroy(m_device);
            if (m_pipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
                m_pipelineLayout = VK_NULL_HANDLE;
            }

            DestroyVertexBuffer(m_device, m_defaultInstanceVB);
        }
    }

    void TrianglesRenderPassModule::createPipeline(VulkanContext &ctx, VkRenderPass pass)
    {
        PipelineCreateInfo pci{};
        pci.device = ctx.GetDevice();
        pci.renderPass = pass;
        pci.subpass = 0;

        // Load shader modules
        VkShaderModule vert = Pipeline::createShaderModuleFromFile(pci.device, "shaders/triangle.vert.spv");
        VkShaderModule frag = Pipeline::createShaderModuleFromFile(pci.device, "shaders/triangle.frag.spv");

        VkPipelineShaderStageCreateInfo vs{};
        vs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vs.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vs.module = vert;
        vs.pName = "main";

        VkPipelineShaderStageCreateInfo fs{};
        fs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fs.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fs.module = frag;
        fs.pName = "main";

        pci.shaderStages = {vs, fs};

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(float) * 2;

        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &pcRange;
        plInfo.setLayoutCount = 0;
        plInfo.pSetLayouts = nullptr;

        if (vkCreatePipelineLayout(pci.device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            throw std::runtime_error("Triangles: failed to create pipeline layout");
        }

        // Ensure the PipelineCreateInfo uses the layout we just created
        pci.pipelineLayout = m_pipelineLayout;

        // Vertex input:
        //  binding 0: per-vertex (vec2 pos + vec3 color) => 5 floats
        //  binding 1: per-instance (vec2 offset + vec3 color) => 5 floats
        VkVertexInputBindingDescription bindingDescs[2]{};
        bindingDescs[0].binding = 0;
        bindingDescs[0].stride = static_cast<uint32_t>(sizeof(float) * 5);
        bindingDescs[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        bindingDescs[1].binding = 1;
        bindingDescs[1].stride = static_cast<uint32_t>(sizeof(float) * 5);
        bindingDescs[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        VkVertexInputAttributeDescription attrs[4]{};
        // location 0: vec2 position
        attrs[0].location = 0;
        attrs[0].binding = 0;
        attrs[0].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[0].offset = 0;
        // location 1: vec3 color
        attrs[1].location = 1;
        attrs[1].binding = 0;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = static_cast<uint32_t>(sizeof(float) * 2);

        // location 2: vec2 instance offset
        attrs[2].location = 2;
        attrs[2].binding = 1;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = 0;

        // location 3: vec3 instance color
        attrs[3].location = 3;
        attrs[3].binding = 1;
        attrs[3].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[3].offset = static_cast<uint32_t>(sizeof(float) * 2);

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 2;
        vi.pVertexBindingDescriptions = bindingDescs;
        vi.vertexAttributeDescriptionCount = 4;
        vi.pVertexAttributeDescriptions = attrs;
        pci.vertexInput = vi;
        pci.vertexInputProvided = true;

        // Input assembly: triangle list
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ia.primitiveRestartEnable = VK_FALSE;
        pci.inputAssembly = ia;
        pci.inputAssemblyProvided = true;

        // Dynamic viewport/scissor
        pci.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        // Rasterization: no cull to see both orientations
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        pci.rasterization = rs;
        pci.rasterizationProvided = true;

        // Multisample default
        // Color blend default (no blending)

        VkResult r = m_pipeline.create(pci);

        // Destroy temp shader modules regardless of success
        vkDestroyShaderModule(pci.device, vert, nullptr);
        vkDestroyShaderModule(pci.device, frag, nullptr);

        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("TrianglesRenderPassModule: failed to create pipeline");
        }
    }

} // namespace Engine
