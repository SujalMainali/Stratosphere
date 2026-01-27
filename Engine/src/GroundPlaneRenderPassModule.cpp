#include "Engine/GroundPlaneRenderPassModule.h"

#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "Engine/PerformanceMonitor.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace Engine
{
    static bool findMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags properties, uint32_t &typeIndex)
    {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            {
                typeIndex = i;
                return true;
            }
        }
        return false;
    }

    void GroundPlaneRenderPassModule::onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs)
    {
        (void)pass;

        m_device = ctx.GetDevice();
        m_physicalDevice = ctx.GetPhysicalDevice();
        m_extent = ctx.GetSwapChain() ? ctx.GetSwapChain()->GetExtent() : VkExtent2D{};

        const size_t frameCount = (fbs.empty() ? 1u : fbs.size());

        if (!createCameraResources(ctx, frameCount))
            throw std::runtime_error("GroundPlaneRenderPassModule: failed to create camera resources");

        if (!createInstanceResources(ctx, frameCount))
            throw std::runtime_error("GroundPlaneRenderPassModule: failed to create instance resources");

        if (!createMaterialResources(ctx, frameCount))
            throw std::runtime_error("GroundPlaneRenderPassModule: failed to create material resources");

        if (!createGeometryResources(ctx, frameCount))
            throw std::runtime_error("GroundPlaneRenderPassModule: failed to create geometry resources");

        // Pipeline: reuse smodel shaders (camera set + material set + push constants)
        PipelineCreateInfo pci{};
        pci.device = ctx.GetDevice();
        pci.renderPass = pass;
        pci.subpass = 0;

        VkShaderModule vert = Pipeline::createShaderModuleFromFile(pci.device, "shaders/smodel.vert.spv");
        VkShaderModule frag = Pipeline::createShaderModuleFromFile(pci.device, "shaders/smodel.frag.spv");

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

        // Vertex input matches SModelRenderPassModule
        std::array<VkVertexInputBindingDescription, 2> bindingDescs{};
        bindingDescs[0].binding = 0;
        bindingDescs[0].stride = 48;
        bindingDescs[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        bindingDescs[1].binding = 1;
        bindingDescs[1].stride = sizeof(glm::mat4);
        bindingDescs[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;

        std::array<VkVertexInputAttributeDescription, 8> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32};
        attrs[4] = {4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
        attrs[5] = {5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 16};
        attrs[6] = {6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 32};
        attrs[7] = {7, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 48};

        VkPipelineVertexInputStateCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vi.vertexBindingDescriptionCount = static_cast<uint32_t>(bindingDescs.size());
        vi.pVertexBindingDescriptions = bindingDescs.data();
        vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
        vi.pVertexAttributeDescriptions = attrs.data();
        pci.vertexInput = vi;
        pci.vertexInputProvided = true;

        VkPipelineInputAssemblyStateCreateInfo ia{};
        ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        ia.primitiveRestartEnable = VK_FALSE;
        pci.inputAssembly = ia;
        pci.inputAssemblyProvided = true;

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

        VkPipelineDepthStencilStateCreateInfo ds{};
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_LESS;
        ds.depthBoundsTestEnable = VK_FALSE;
        ds.stencilTestEnable = VK_FALSE;
        pci.depthStencil = ds;
        pci.depthStencilProvided = true;

        pci.dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(PushConstants);

        pci.descriptorSetLayouts = {m_cameraSetLayout, m_materialSetLayout};
        pci.pushConstantRanges = {pcRange};

        VkResult r = m_pipeline.create(pci);

        vkDestroyShaderModule(pci.device, vert, nullptr);
        vkDestroyShaderModule(pci.device, frag, nullptr);

        if (r != VK_SUCCESS)
            throw std::runtime_error("GroundPlaneRenderPassModule: failed to create pipeline");
    }

    void GroundPlaneRenderPassModule::onResize(VulkanContext &ctx, VkExtent2D newExtent)
    {
        (void)ctx;
        m_extent = newExtent;
    }

    void GroundPlaneRenderPassModule::onDestroy(VulkanContext &ctx)
    {
        (void)ctx;

        if (m_device == VK_NULL_HANDLE)
            return;

        m_pipeline.destroy(m_device);

        destroyGeometryResources();
        destroyMaterialResources();
        destroyInstanceResources();
        destroyCameraResources();

        m_device = VK_NULL_HANDLE;
        m_physicalDevice = VK_NULL_HANDLE;
        m_extent = {};
    }

    bool GroundPlaneRenderPassModule::createCameraResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyCameraResources();
        if (frameCount == 0)
            frameCount = 1;

        VkDescriptorSetLayoutBinding camBinding{};
        camBinding.binding = 0;
        camBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        camBinding.descriptorCount = 1;
        camBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &camBinding;

        if (vkCreateDescriptorSetLayout(ctx.GetDevice(), &dsl, nullptr, &m_cameraSetLayout) != VK_SUCCESS)
            return false;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSize.descriptorCount = static_cast<uint32_t>(frameCount);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(frameCount);
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        if (vkCreateDescriptorPool(ctx.GetDevice(), &poolInfo, nullptr, &m_cameraPool) != VK_SUCCESS)
            return false;

        std::vector<VkDescriptorSetLayout> layouts(frameCount, m_cameraSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = m_cameraPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(frameCount);
        allocInfo.pSetLayouts = layouts.data();

        std::vector<VkDescriptorSet> sets(frameCount, VK_NULL_HANDLE);
        if (vkAllocateDescriptorSets(ctx.GetDevice(), &allocInfo, sets.data()) != VK_SUCCESS)
            return false;

        m_cameraFrames.resize(frameCount);

        const VkDeviceSize bufSize = sizeof(CameraUBO);
        for (size_t i = 0; i < frameCount; ++i)
        {
            CameraFrame &cf = m_cameraFrames[i];
            cf.set = sets[i];

            VkBufferCreateInfo binfo{};
            binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            binfo.size = bufSize;
            binfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &binfo, nullptr, &cf.buffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.buffer, &memReq);

            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = memReq.size;
            uint32_t memType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
                return false;
            mai.memoryTypeIndex = memType;

            if (vkAllocateMemory(ctx.GetDevice(), &mai, nullptr, &cf.memory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.buffer, cf.memory, 0);

            VkDescriptorBufferInfo dbi{};
            dbi.buffer = cf.buffer;
            dbi.offset = 0;
            dbi.range = bufSize;

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = cf.set;
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            write.descriptorCount = 1;
            write.pBufferInfo = &dbi;

            vkUpdateDescriptorSets(ctx.GetDevice(), 1, &write, 0, nullptr);
        }

        return true;
    }

    void GroundPlaneRenderPassModule::destroyCameraResources()
    {
        for (auto &cf : m_cameraFrames)
        {
            if (cf.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.buffer, nullptr);
                cf.buffer = VK_NULL_HANDLE;
            }
            if (cf.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.memory, nullptr);
                cf.memory = VK_NULL_HANDLE;
            }
            cf.set = VK_NULL_HANDLE;
        }
        m_cameraFrames.clear();

        if (m_cameraPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_cameraPool, nullptr);
            m_cameraPool = VK_NULL_HANDLE;
        }
        if (m_cameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_cameraSetLayout, nullptr);
            m_cameraSetLayout = VK_NULL_HANDLE;
        }
    }

    bool GroundPlaneRenderPassModule::createInstanceResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyInstanceResources();
        if (frameCount == 0)
            frameCount = 1;

        m_instanceFrames.resize(frameCount);

        const VkDeviceSize bufSize = sizeof(glm::mat4);
        for (size_t i = 0; i < frameCount; ++i)
        {
            InstanceFrame &fr = m_instanceFrames[i];

            VkBufferCreateInfo binfo{};
            binfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            binfo.size = bufSize;
            binfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            binfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &binfo, nullptr, &fr.buffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements memReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), fr.buffer, &memReq);

            VkMemoryAllocateInfo mai{};
            mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            mai.allocationSize = memReq.size;
            uint32_t memType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), memReq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, memType))
                return false;
            mai.memoryTypeIndex = memType;

            if (vkAllocateMemory(ctx.GetDevice(), &mai, nullptr, &fr.memory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), fr.buffer, fr.memory, 0);

            fr.mapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), fr.memory, 0, VK_WHOLE_SIZE, 0, &fr.mapped) != VK_SUCCESS)
                return false;
        }

        return true;
    }

    void GroundPlaneRenderPassModule::destroyInstanceResources()
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        for (auto &fr : m_instanceFrames)
        {
            if (fr.mapped && fr.memory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, fr.memory);
                fr.mapped = nullptr;
            }
            if (fr.buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, fr.buffer, nullptr);
                fr.buffer = VK_NULL_HANDLE;
            }
            if (fr.memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, fr.memory, nullptr);
                fr.memory = VK_NULL_HANDLE;
            }
        }
        m_instanceFrames.clear();
    }

    bool GroundPlaneRenderPassModule::createMaterialResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyMaterialResources();

        if (frameCount == 0)
            frameCount = 1;

        VkDescriptorSetLayoutBinding samplerBinding{};
        samplerBinding.binding = 0;
        samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerBinding.descriptorCount = 1;
        samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dsl.bindingCount = 1;
        dsl.pBindings = &samplerBinding;

        if (vkCreateDescriptorSetLayout(ctx.GetDevice(), &dsl, nullptr, &m_materialSetLayout) != VK_SUCCESS)
            return false;

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSize.descriptorCount = static_cast<uint32_t>(frameCount);

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(frameCount);
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &poolSize;

        if (vkCreateDescriptorPool(ctx.GetDevice(), &poolInfo, nullptr, &m_materialPool) != VK_SUCCESS)
            return false;

        std::vector<VkDescriptorSetLayout> layouts(frameCount, m_materialSetLayout);
        VkDescriptorSetAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        alloc.descriptorPool = m_materialPool;
        alloc.descriptorSetCount = static_cast<uint32_t>(frameCount);
        alloc.pSetLayouts = layouts.data();

        m_materialSets.resize(frameCount, VK_NULL_HANDLE);
        if (vkAllocateDescriptorSets(ctx.GetDevice(), &alloc, m_materialSets.data()) != VK_SUCCESS)
            return false;

        // Write the texture once for all frames.
        if (!m_assets || !m_baseColorTexture.isValid())
            return true; // pass can be enabled later when texture becomes valid

        TextureAsset *tex = m_assets->getTexture(m_baseColorTexture);
        if (!tex || tex->getView() == VK_NULL_HANDLE || tex->getSampler() == VK_NULL_HANDLE)
            return true;

        for (size_t i = 0; i < frameCount; ++i)
        {
            VkDescriptorImageInfo di{};
            di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            di.imageView = tex->getView();
            di.sampler = tex->getSampler();

            VkWriteDescriptorSet write{};
            write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            write.dstSet = m_materialSets[i];
            write.dstBinding = 0;
            write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.descriptorCount = 1;
            write.pImageInfo = &di;
            vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);
        }

        return true;
    }

    void GroundPlaneRenderPassModule::destroyMaterialResources()
    {
        if (m_materialPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_materialPool, nullptr);
            m_materialPool = VK_NULL_HANDLE;
        }
        if (m_materialSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(m_device, m_materialSetLayout, nullptr);
            m_materialSetLayout = VK_NULL_HANDLE;
        }
        m_materialSets.clear();
    }

    bool GroundPlaneRenderPassModule::createGeometryResources(VulkanContext &ctx, size_t frameCount)
    {
        destroyGeometryResources();
        if (frameCount == 0)
            frameCount = 1;

        // Index buffer (shared)
        const uint16_t indices[6] = {0, 1, 2, 2, 1, 3};
        if (CreateOrUpdateIndexBuffer(ctx.GetDevice(), ctx.GetPhysicalDevice(), indices, sizeof(indices), m_planeIB) != VK_SUCCESS)
            return false;

        m_planeVB.resize(frameCount);
        // Initialize vertex buffers with some data.
        for (size_t i = 0; i < frameCount; ++i)
        {
            PlaneVertex verts[4]{};
            if (CreateOrUpdateVertexBuffer(ctx.GetDevice(), ctx.GetPhysicalDevice(), verts, sizeof(verts), m_planeVB[i]) != VK_SUCCESS)
                return false;
        }

        return true;
    }

    void GroundPlaneRenderPassModule::destroyGeometryResources()
    {
        if (m_device == VK_NULL_HANDLE)
            return;

        for (auto &vb : m_planeVB)
            DestroyVertexBuffer(m_device, vb);
        m_planeVB.clear();

        DestroyIndexBuffer(m_device, m_planeIB);
    }

    void GroundPlaneRenderPassModule::updatePlaneForFrame(uint32_t frameIndex)
    {
        if (m_planeVB.empty())
            return;

        const uint32_t idx = frameIndex % static_cast<uint32_t>(m_planeVB.size());

        const float half = std::max(1.0f, m_halfSize);
        const float tile = std::max(0.001f, m_tileWorldSize);

        float cx = 0.0f;
        float cz = 0.0f;
        if (m_camera)
        {
            const glm::vec3 p = m_camera->GetPosition();
            cx = p.x;
            cz = p.z;
        }

        const float x0 = cx - half;
        const float x1 = cx + half;
        const float z0 = cz - half;
        const float z1 = cz + half;

        auto uv = [&](float x, float z) -> glm::vec2
        {
            return glm::vec2(x / tile, z / tile);
        };

        PlaneVertex verts[4]{};
        const glm::vec3 n(0.0f, 1.0f, 0.0f);
        const glm::vec4 t(1.0f, 0.0f, 0.0f, 1.0f);

        verts[0] = {glm::vec3(x0, 0.0f, z0), n, uv(x0, z0), t};
        verts[1] = {glm::vec3(x1, 0.0f, z0), n, uv(x1, z0), t};
        verts[2] = {glm::vec3(x0, 0.0f, z1), n, uv(x0, z1), t};
        verts[3] = {glm::vec3(x1, 0.0f, z1), n, uv(x1, z1), t};

        (void)CreateOrUpdateVertexBuffer(m_device, m_physicalDevice, verts, sizeof(verts), m_planeVB[idx]);
    }

    void GroundPlaneRenderPassModule::record(FrameContext &frameCtx, VkCommandBuffer cmd)
    {
        if (!m_enabled)
            return;
        if (m_device == VK_NULL_HANDLE)
            return;
        if (m_extent.width == 0 || m_extent.height == 0)
            return;
        if (!m_assets || !m_baseColorTexture.isValid())
            return;
        if (m_materialSets.empty())
            return;

        VkViewport vp{0.0f, 0.0f, static_cast<float>(m_extent.width), static_cast<float>(m_extent.height), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {m_extent.width, m_extent.height}};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);

        // Update camera UBO
        const uint32_t camIndex = (!m_cameraFrames.empty()) ? (frameCtx.frameIndex % static_cast<uint32_t>(m_cameraFrames.size())) : 0;
        CameraFrame *camFrame = (!m_cameraFrames.empty()) ? &m_cameraFrames[camIndex] : nullptr;
        if (camFrame && camFrame->memory != VK_NULL_HANDLE)
        {
            CameraUBO ubo{};
            const float aspect = (m_extent.height > 0) ? (static_cast<float>(m_extent.width) / static_cast<float>(m_extent.height)) : 1.0f;
            if (m_camera)
            {
                m_camera->SetAspect(aspect);
                ubo.view = m_camera->GetViewMatrix();
                ubo.proj = m_camera->GetProjectionMatrix();
            }
            else
            {
                glm::mat4 view = glm::lookAt(glm::vec3(0, 0, 3), glm::vec3(0, 0, 0), glm::vec3(0, 1, 0));
                glm::mat4 proj = glm::perspective(glm::radians(60.0f), aspect, 0.1f, 100.0f);
                proj[1][1] *= -1.0f;
                ubo.view = view;
                ubo.proj = proj;
            }

            void *mapped = nullptr;
            if (vkMapMemory(m_device, camFrame->memory, 0, sizeof(CameraUBO), 0, &mapped) == VK_SUCCESS && mapped)
            {
                std::memcpy(mapped, &ubo, sizeof(CameraUBO));
                vkUnmapMemory(m_device, camFrame->memory);
            }
        }

        // Update per-frame instance transform (identity)
        const uint32_t instIndex = (!m_instanceFrames.empty()) ? (frameCtx.frameIndex % static_cast<uint32_t>(m_instanceFrames.size())) : 0;
        InstanceFrame *instFrame = (!m_instanceFrames.empty()) ? &m_instanceFrames[instIndex] : nullptr;
        if (!instFrame || !instFrame->mapped)
            return;
        const glm::mat4 I(1.0f);
        std::memcpy(instFrame->mapped, &I, sizeof(glm::mat4));

        // Update plane vertices (centered around camera; UVs in world-space)
        updatePlaneForFrame(frameCtx.frameIndex);

        // Push constants: opaque material
        m_pc.model = glm::mat4(1.0f);
        m_pc.baseColorFactor = glm::vec4(1.0f);
        m_pc.materialParams = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f); // alphaCutoff=0.5, alphaMode=Opaque

        m_pipeline.bind(cmd);

        VkDescriptorSet camSet = (!m_cameraFrames.empty()) ? m_cameraFrames[camIndex].set : VK_NULL_HANDLE;
        VkDescriptorSet matSet = m_materialSets[frameCtx.frameIndex % static_cast<uint32_t>(m_materialSets.size())];
        VkDescriptorSet sets[2] = {camSet, matSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.getLayout(), 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &m_pc);

        const uint32_t vbIndex = frameCtx.frameIndex % static_cast<uint32_t>(m_planeVB.size());
        VkBuffer vbs[2] = {m_planeVB[vbIndex].buffer, instFrame->buffer};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, m_planeIB.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
        DrawCallCounter::increment();
    }
}
