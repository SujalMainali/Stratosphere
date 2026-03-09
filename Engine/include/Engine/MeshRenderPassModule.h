#pragma once
#include "Engine/Renderer.h"
#include "Engine/Pipeline.h"
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>
#include <glm/glm.hpp>
#include "assets/Handles.h"
#include "assets/MeshAsset.h"
#include "assets/ModelAsset.h"
#include "assets/MaterialAsset.h"
#include "assets/TextureAsset.h"

namespace Engine
{
    class Camera;
    class AssetManager;

    class MeshRenderPassModule : public RenderPassModule
    {
    public:
        MeshRenderPassModule() = default;
        ~MeshRenderPassModule() override;

        const char *getDebugName() const override { return "Mesh"; }

        void setCamera(Camera *camera);
        void setAssetManager(AssetManager *assetManager);
        void setModelHandle(ModelHandle model);
        void setEnabled(bool en) { m_enabled = en; }

        void onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs) override;
        void record(FrameContext &frameCtx, VkCommandBuffer cmd) override;
        void onResize(VulkanContext &ctx, VkExtent2D newExtent) override;
        void onDestroy(VulkanContext &ctx) override;

    private:
        // Initialization
        void createDescriptorLayouts();
        void createDescriptorPool(size_t materialCount);
        void createCameraUBO();
        void createPipelinesForModel(VulkanContext &ctx, VkRenderPass pass);
        void createMaterialDescriptors();
        void createDummyTexture(VulkanContext &ctx);

        // Per-frame
        void updateCameraUBO();
        void drawNode(VkCommandBuffer cmd, size_t primitiveIndex);

        // Helpers
        glm::mat4 computeModelMatrix(MeshAsset *mesh);
        uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) const;
        void cleanupResources();

        // Vulkan handles
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_phys = VK_NULL_HANDLE;
        VkExtent2D m_extent{};

        // Pipelines per vertex stride
        std::unordered_map<uint32_t, Pipeline> m_pipelines;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;

        // Descriptors
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_cameraSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorSet m_cameraDescriptorSet = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_materialDescriptorSets;

        // Camera UBO
        VkBuffer m_cameraUBO = VK_NULL_HANDLE;
        VkDeviceMemory m_cameraUBOMemory = VK_NULL_HANDLE;
        void *m_cameraUBOMapped = nullptr;

        // Per-material UBOs
        std::vector<VkBuffer> m_materialUBOs;
        std::vector<VkDeviceMemory> m_materialUBOMemories;

        // Dummy texture for missing textures
        VkImage m_dummyImage = VK_NULL_HANDLE;
        VkDeviceMemory m_dummyImageMemory = VK_NULL_HANDLE;
        VkImageView m_dummyImageView = VK_NULL_HANDLE;
        VkSampler m_dummySampler = VK_NULL_HANDLE;

        // Upload helper
        VkCommandPool m_uploadPool = VK_NULL_HANDLE;

        // Scene data
        Camera *m_camera = nullptr;
        AssetManager *m_assetManager = nullptr;
        ModelHandle m_modelHandle{};
        bool m_enabled = true;
    };

} // namespace Engine