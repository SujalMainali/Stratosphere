#include "Engine/MeshRenderPassModule.h"
#include "Engine/Pipeline.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "Engine/PerformanceMonitor.h"
#include <stdexcept>
#include <iostream>

namespace Engine
{

    MeshRenderPassModule::~MeshRenderPassModule() { /* resources freed in onDestroy */ }

    void MeshRenderPassModule::setMesh(const MeshBinding &binding)
    {
        m_binding = binding;
    }

    void MeshRenderPassModule::onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs)
    {
        (void)fbs;
        m_device = ctx.GetDevice();
        m_extent = ctx.GetSwapChain()->GetExtent();

        createPipeline(ctx, pass);
    }

    void MeshRenderPassModule::createPipeline(VulkanContext &ctx, VkRenderPass pass)
    {
        PipelineCreateInfo pci{};
        pci.device = ctx.GetDevice();
        pci.renderPass = pass;
        pci.subpass = 0;

        // Load shader modules (place SPIR-V in Engine/Shaders/)
        VkShaderModule vert = Pipeline::createShaderModuleFromFile(pci.device, "shaders/mesh.vert.spv");
        VkShaderModule frag = Pipeline::createShaderModuleFromFile(pci.device, "shaders/mesh.frag.spv");
        if (vert == VK_NULL_HANDLE || frag == VK_NULL_HANDLE)
        {
            throw std::runtime_error("Mesh: failed to load shader modules");
        }

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

        // Pipeline layout (no descriptors/push constants for minimal case)
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 0;
        plInfo.pSetLayouts = nullptr;
        plInfo.pushConstantRangeCount = 0;
        plInfo.pPushConstantRanges = nullptr;
        if (vkCreatePipelineLayout(pci.device, &plInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
        {
            vkDestroyShaderModule(pci.device, vert, nullptr);
            vkDestroyShaderModule(pci.device, frag, nullptr);
            throw std::runtime_error("Mesh: failed to create pipeline layout");
        }
        pci.pipelineLayout = m_pipelineLayout;

        // Vertex input: binding 0, stride 32; pos3, norm3, uv2
        VkVertexInputBindingDescription bindingDesc{};
        bindingDesc.binding = 0;
        bindingDesc.stride = 32;
        bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        VkVertexInputAttributeDescription attrs[3]{};
        attrs[0].location = 0;
        attrs[0].binding = 0;
        attrs[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[0].offset = 0;
        attrs[1].location = 1;
        attrs[1].binding = 0;
        attrs[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attrs[1].offset = 12;
        attrs[2].location = 2;
        attrs[2].binding = 0;
        attrs[2].format = VK_FORMAT_R32G32_SFLOAT;
        attrs[2].offset = 24;

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = 1;
        vi.pVertexBindingDescriptions = &bindingDesc;
        vi.vertexAttributeDescriptionCount = 3;
        vi.pVertexAttributeDescriptions = attrs;
        pci.vertexInput = vi;
        pci.vertexInputProvided = true;

        // Input assembly
        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ia.primitiveRestartEnable = VK_FALSE;
        pci.inputAssembly = ia;
        pci.inputAssemblyProvided = true;

        // Rasterization: disable culling for now (OBJ winding can vary; no camera yet)
        VkPipelineRasterizationStateCreateInfo rs{};
        rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.depthClampEnable = VK_FALSE;
        rs.rasterizerDiscardEnable = VK_FALSE;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.depthBiasEnable = VK_FALSE;
        pci.rasterization = rs;
        pci.rasterizationProvided = true;

        // Dynamic viewport/scissor
        pci.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkResult r = m_pipeline.create(pci);

        vkDestroyShaderModule(pci.device, vert, nullptr);
        vkDestroyShaderModule(pci.device, frag, nullptr);

        if (r != VK_SUCCESS)
        {
            throw std::runtime_error("MeshRenderPassModule: failed to create pipeline");
        }
    }

    void MeshRenderPassModule::record(FrameContext &frameCtx, VkCommandBuffer cmd)
    {
        (void)frameCtx;

        if (!m_enabled)
            return;

        // Use m_extent set during onCreate (or via onResize)
        if (m_extent.width == 0 || m_extent.height == 0)
        {
            // Guard: avoid invalid viewport if extent wasn't set yet
            return;
        }

        m_pipeline.bind(cmd);

        VkViewport vp{0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_extent.width, m_extent.height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        if (m_binding.vertexBuffer == VK_NULL_HANDLE || m_binding.indexBuffer == VK_NULL_HANDLE || m_binding.indexCount == 0)
        {
            std::cout << "MeshRenderPassModule: warning - no mesh bound or index count is zero, skipping draw\n";
            return;
        }

        VkDeviceSize offsets[1] = {m_binding.vertexOffset};
        vkCmdBindVertexBuffers(cmd, 0, 1, &m_binding.vertexBuffer, offsets);
        vkCmdBindIndexBuffer(cmd, m_binding.indexBuffer, m_binding.indexOffset, m_binding.indexType);

        vkCmdDrawIndexed(cmd, m_binding.indexCount, 1, 0, 0, 0);
        DrawCallCounter::increment();
    }

    void MeshRenderPassModule::onResize(VulkanContext &ctx, VkExtent2D newExtent)
    {
        m_extent = newExtent;
    }

    void MeshRenderPassModule::destroyResources()
    {
        if (m_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);
            m_pipelineLayout = VK_NULL_HANDLE;
        }
        m_pipeline.destroy(m_device);
    }

    void MeshRenderPassModule::onDestroy(VulkanContext &ctx)
    {
        (void)ctx;
        destroyResources();
    }

} // namespace Engine