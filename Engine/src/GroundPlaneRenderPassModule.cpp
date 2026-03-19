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

        // Vertex input matches SModelRenderPassModule (VertexPNTTJW). Instance data comes from SSBOs.
        std::array<VkVertexInputBindingDescription, 1> bindingDescs{};
        bindingDescs[0].binding = 0;
        bindingDescs[0].stride = 72;
        bindingDescs[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 6> attrs{};
        attrs[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attrs[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12};
        attrs[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, 24};
        attrs[3] = {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32};

        // Skinning inputs (unused for ground, but required by smodel.vert interface)
        attrs[4] = {8, 0, VK_FORMAT_R16G16B16A16_UINT, 48};
        attrs[5] = {9, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 56};

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

        VkDescriptorSetLayoutBinding paletteBinding{};
        paletteBinding.binding = 1;
        paletteBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        paletteBinding.descriptorCount = 1;
        paletteBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding jointPaletteBinding{};
        jointPaletteBinding.binding = 2;
        jointPaletteBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        jointPaletteBinding.descriptorCount = 1;
        jointPaletteBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding instanceWorldBinding{};
        instanceWorldBinding.binding = 3;
        instanceWorldBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        instanceWorldBinding.descriptorCount = 1;
        instanceWorldBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding activeSlotsBinding{};
        activeSlotsBinding.binding = 4;
        activeSlotsBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        activeSlotsBinding.descriptorCount = 1;
        activeSlotsBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo dsl{};
        dsl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        VkDescriptorSetLayoutBinding bindings[5] = {camBinding, paletteBinding, jointPaletteBinding, instanceWorldBinding, activeSlotsBinding};
        dsl.bindingCount = 5;
        dsl.pBindings = bindings;

        if (vkCreateDescriptorSetLayout(ctx.GetDevice(), &dsl, nullptr, &m_cameraSetLayout) != VK_SUCCESS)
            return false;

        VkDescriptorPoolSize poolSizes[2]{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[0].descriptorCount = static_cast<uint32_t>(frameCount);
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        poolSizes[1].descriptorCount = static_cast<uint32_t>(frameCount) * 4u; // node palette + joint palette + instance worlds + active slots

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = static_cast<uint32_t>(frameCount);
        poolInfo.poolSizeCount = 2;
        poolInfo.pPoolSizes = poolSizes;

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

            // Palette SSBO: allocate a tiny buffer (identity matrix) to satisfy smodel.vert.
            cf.paletteCapacityMatrices = 4;

            VkBufferCreateInfo pbinfo{};
            pbinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            pbinfo.size = static_cast<VkDeviceSize>(cf.paletteCapacityMatrices) * sizeof(glm::mat4);
            pbinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            pbinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &pbinfo, nullptr, &cf.paletteBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements pmemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.paletteBuffer, &pmemReq);

            VkMemoryAllocateInfo pmai{};
            pmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            pmai.allocationSize = pmemReq.size;
            uint32_t pmemType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), pmemReq.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, pmemType))
                return false;
            pmai.memoryTypeIndex = pmemType;

            if (vkAllocateMemory(ctx.GetDevice(), &pmai, nullptr, &cf.paletteMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.paletteBuffer, cf.paletteMemory, 0);

            cf.paletteMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.paletteMemory, 0, VK_WHOLE_SIZE, 0, &cf.paletteMapped) != VK_SUCCESS)
                return false;

            if (cf.paletteMapped)
            {
                const glm::mat4 I(1.0f);
                std::memcpy(cf.paletteMapped, &I, sizeof(glm::mat4));
            }

            // Joint palette SSBO: allocate a tiny buffer (identity matrix) to satisfy smodel.vert.
            cf.jointPaletteCapacityMatrices = 4;

            VkBufferCreateInfo jbinfo{};
            jbinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            jbinfo.size = static_cast<VkDeviceSize>(cf.jointPaletteCapacityMatrices) * sizeof(glm::mat4);
            jbinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            jbinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &jbinfo, nullptr, &cf.jointPaletteBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements jmemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.jointPaletteBuffer, &jmemReq);

            VkMemoryAllocateInfo jmai{};
            jmai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            jmai.allocationSize = jmemReq.size;
            uint32_t jmemType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), jmemReq.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, jmemType))
                return false;
            jmai.memoryTypeIndex = jmemType;

            if (vkAllocateMemory(ctx.GetDevice(), &jmai, nullptr, &cf.jointPaletteMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.jointPaletteBuffer, cf.jointPaletteMemory, 0);

            cf.jointPaletteMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.jointPaletteMemory, 0, VK_WHOLE_SIZE, 0, &cf.jointPaletteMapped) != VK_SUCCESS)
                return false;

            if (cf.jointPaletteMapped)
            {
                const glm::mat4 I(1.0f);
                std::memcpy(cf.jointPaletteMapped, &I, sizeof(glm::mat4));
            }

            // Instance world SSBO: single slot (identity) to satisfy smodel.vert.
            cf.instanceWorldCapacitySlots = 1;
            VkBufferCreateInfo iwinfo{};
            iwinfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            iwinfo.size = static_cast<VkDeviceSize>(cf.instanceWorldCapacitySlots) * sizeof(glm::mat4);
            iwinfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            iwinfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &iwinfo, nullptr, &cf.instanceWorldBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements iwMemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.instanceWorldBuffer, &iwMemReq);

            VkMemoryAllocateInfo iwMai{};
            iwMai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            iwMai.allocationSize = iwMemReq.size;
            uint32_t iwMemType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), iwMemReq.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, iwMemType))
                return false;
            iwMai.memoryTypeIndex = iwMemType;

            if (vkAllocateMemory(ctx.GetDevice(), &iwMai, nullptr, &cf.instanceWorldMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.instanceWorldBuffer, cf.instanceWorldMemory, 0);

            cf.instanceWorldMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.instanceWorldMemory, 0, VK_WHOLE_SIZE, 0, &cf.instanceWorldMapped) != VK_SUCCESS)
                return false;
            if (cf.instanceWorldMapped)
            {
                const glm::mat4 I(1.0f);
                std::memcpy(cf.instanceWorldMapped, &I, sizeof(glm::mat4));
            }

            // Active slots SSBO: one entry mapping instance 0 -> slot 0.
            cf.activeSlotsCapacity = 1;
            VkBufferCreateInfo asInfo{};
            asInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            asInfo.size = static_cast<VkDeviceSize>(cf.activeSlotsCapacity) * sizeof(uint32_t);
            asInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            asInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            if (vkCreateBuffer(ctx.GetDevice(), &asInfo, nullptr, &cf.activeSlotsBuffer) != VK_SUCCESS)
                return false;

            VkMemoryRequirements asMemReq{};
            vkGetBufferMemoryRequirements(ctx.GetDevice(), cf.activeSlotsBuffer, &asMemReq);

            VkMemoryAllocateInfo asMai{};
            asMai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            asMai.allocationSize = asMemReq.size;
            uint32_t asMemType = 0;
            if (!findMemoryType(ctx.GetPhysicalDevice(), asMemReq.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, asMemType))
                return false;
            asMai.memoryTypeIndex = asMemType;

            if (vkAllocateMemory(ctx.GetDevice(), &asMai, nullptr, &cf.activeSlotsMemory) != VK_SUCCESS)
                return false;

            vkBindBufferMemory(ctx.GetDevice(), cf.activeSlotsBuffer, cf.activeSlotsMemory, 0);

            cf.activeSlotsMapped = nullptr;
            if (vkMapMemory(ctx.GetDevice(), cf.activeSlotsMemory, 0, VK_WHOLE_SIZE, 0, &cf.activeSlotsMapped) != VK_SUCCESS)
                return false;
            if (cf.activeSlotsMapped)
            {
                const uint32_t slot = 0;
                std::memcpy(cf.activeSlotsMapped, &slot, sizeof(uint32_t));
            }

            VkDescriptorBufferInfo dbi{};
            dbi.buffer = cf.buffer;
            dbi.offset = 0;
            dbi.range = bufSize;

            VkDescriptorBufferInfo pbi{};
            pbi.buffer = cf.paletteBuffer;
            pbi.offset = 0;
            pbi.range = static_cast<VkDeviceSize>(cf.paletteCapacityMatrices) * sizeof(glm::mat4);

            VkDescriptorBufferInfo jbi{};
            jbi.buffer = cf.jointPaletteBuffer;
            jbi.offset = 0;
            jbi.range = static_cast<VkDeviceSize>(cf.jointPaletteCapacityMatrices) * sizeof(glm::mat4);

            VkDescriptorBufferInfo iwbi{};
            iwbi.buffer = cf.instanceWorldBuffer;
            iwbi.offset = 0;
            iwbi.range = static_cast<VkDeviceSize>(cf.instanceWorldCapacitySlots) * sizeof(glm::mat4);

            VkDescriptorBufferInfo asbi{};
            asbi.buffer = cf.activeSlotsBuffer;
            asbi.offset = 0;
            asbi.range = static_cast<VkDeviceSize>(cf.activeSlotsCapacity) * sizeof(uint32_t);

            VkWriteDescriptorSet writes[5]{};
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = cf.set;
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &dbi;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = cf.set;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].descriptorCount = 1;
            writes[1].pBufferInfo = &pbi;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = cf.set;
            writes[2].dstBinding = 2;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].descriptorCount = 1;
            writes[2].pBufferInfo = &jbi;

            writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[3].dstSet = cf.set;
            writes[3].dstBinding = 3;
            writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[3].descriptorCount = 1;
            writes[3].pBufferInfo = &iwbi;

            writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[4].dstSet = cf.set;
            writes[4].dstBinding = 4;
            writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[4].descriptorCount = 1;
            writes[4].pBufferInfo = &asbi;

            vkUpdateDescriptorSets(ctx.GetDevice(), 5, writes, 0, nullptr);
        }

        return true;
    }

    void GroundPlaneRenderPassModule::destroyCameraResources()
    {
        for (auto &cf : m_cameraFrames)
        {
            if (cf.activeSlotsMapped && cf.activeSlotsMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.activeSlotsMemory);
                cf.activeSlotsMapped = nullptr;
            }
            if (cf.activeSlotsBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.activeSlotsBuffer, nullptr);
                cf.activeSlotsBuffer = VK_NULL_HANDLE;
            }
            if (cf.activeSlotsMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.activeSlotsMemory, nullptr);
                cf.activeSlotsMemory = VK_NULL_HANDLE;
            }
            cf.activeSlotsCapacity = 0;

            if (cf.instanceWorldMapped && cf.instanceWorldMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.instanceWorldMemory);
                cf.instanceWorldMapped = nullptr;
            }
            if (cf.instanceWorldBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.instanceWorldBuffer, nullptr);
                cf.instanceWorldBuffer = VK_NULL_HANDLE;
            }
            if (cf.instanceWorldMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.instanceWorldMemory, nullptr);
                cf.instanceWorldMemory = VK_NULL_HANDLE;
            }
            cf.instanceWorldCapacitySlots = 0;

            if (cf.jointPaletteMapped && cf.jointPaletteMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.jointPaletteMemory);
                cf.jointPaletteMapped = nullptr;
            }
            if (cf.jointPaletteBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.jointPaletteBuffer, nullptr);
                cf.jointPaletteBuffer = VK_NULL_HANDLE;
            }
            if (cf.jointPaletteMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.jointPaletteMemory, nullptr);
                cf.jointPaletteMemory = VK_NULL_HANDLE;
            }
            cf.jointPaletteCapacityMatrices = 0;

            if (cf.paletteMapped && cf.paletteMemory != VK_NULL_HANDLE)
            {
                vkUnmapMemory(m_device, cf.paletteMemory);
                cf.paletteMapped = nullptr;
            }
            if (cf.paletteBuffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(m_device, cf.paletteBuffer, nullptr);
                cf.paletteBuffer = VK_NULL_HANDLE;
            }
            if (cf.paletteMemory != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, cf.paletteMemory, nullptr);
                cf.paletteMemory = VK_NULL_HANDLE;
            }
            cf.paletteCapacityMatrices = 0;

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

        // Update plane vertices (centered around camera; UVs in world-space)
        updatePlaneForFrame(frameCtx.frameIndex);

        // Push constants: opaque material
        m_pc.model = glm::mat4(1.0f);
        m_pc.baseColorFactor = glm::vec4(1.0f);
        m_pc.materialParams = glm::vec4(0.5f, 0.0f, 0.0f, 0.0f); // alphaCutoff=0.5, alphaMode=Opaque
        m_pc.nodeInfo = glm::uvec4(0u, 1u, 0u, 0u);

        m_pipeline.bind(cmd);

        VkDescriptorSet camSet = (!m_cameraFrames.empty()) ? m_cameraFrames[camIndex].set : VK_NULL_HANDLE;
        VkDescriptorSet matSet = m_materialSets[frameCtx.frameIndex % static_cast<uint32_t>(m_materialSets.size())];
        VkDescriptorSet sets[2] = {camSet, matSet};
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline.getLayout(), 0, 2, sets, 0, nullptr);
        vkCmdPushConstants(cmd, m_pipeline.getLayout(), VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushConstants), &m_pc);

        const uint32_t vbIndex = frameCtx.frameIndex % static_cast<uint32_t>(m_planeVB.size());
        VkBuffer vbs[1] = {m_planeVB[vbIndex].buffer};
        VkDeviceSize offs[1] = {0};
        vkCmdBindVertexBuffers(cmd, 0, 1, vbs, offs);
        vkCmdBindIndexBuffer(cmd, m_planeIB.buffer, 0, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cmd, 6, 1, 0, 0, 0);
        DrawCallCounter::increment();
    }
}
