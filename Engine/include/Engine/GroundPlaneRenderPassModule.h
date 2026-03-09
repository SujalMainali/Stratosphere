#pragma once

#include "Engine/Renderer.h"

#include "assets/AssetManager.h"
#include "assets/Handles.h"

#include "Engine/Camera.h"
#include "Engine/Pipeline.h"

#include "utils/BufferUtils.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace Engine
{
    // Renders a single ground plane (XZ) with a tiled base-color texture.
    // Uses the existing smodel shaders so we can reuse the texture/material path.
    class GroundPlaneRenderPassModule final : public RenderPassModule
    {
    public:
        GroundPlaneRenderPassModule() = default;
        ~GroundPlaneRenderPassModule() override = default;

        const char *getDebugName() const override { return "GroundPlane"; }

        void setEnabled(bool enabled) { m_enabled = enabled; }
        void setAssets(AssetManager *assets) { m_assets = assets; }
        void setCamera(Camera *camera) { m_camera = camera; }
        void setBaseColorTexture(TextureHandle tex) { m_baseColorTexture = tex; }

        // Half-size (meters) of the quad around the camera.
        void setHalfSize(float halfSize) { m_halfSize = halfSize; }

        // World-space meters per one texture repeat.
        void setTileWorldSize(float metersPerRepeat) { m_tileWorldSize = metersPerRepeat; }

        void onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs) override;
        void record(FrameContext &frameCtx, VkCommandBuffer cmd) override;
        void onResize(VulkanContext &ctx, VkExtent2D newExtent) override;
        void onDestroy(VulkanContext &ctx) override;

    private:
        struct CameraUBO
        {
            glm::mat4 view{1.0f};
            glm::mat4 proj{1.0f};
        };

        struct PushConstants
        {
            glm::mat4 model{1.0f};
            glm::vec4 baseColorFactor{1.0f};
            glm::vec4 materialParams{0.0f}; // x=alphaCutoff, y=alphaMode

            // Matches smodel.vert push constants (x=nodeIndex, y=nodeCount)
            glm::uvec4 nodeInfo{0u, 1u, 0u, 0u};

            // Matches smodel.vert skinInfo (unused for ground)
            glm::uvec4 skinInfo{0u, 0u, 1u, 0u};
        };

        static_assert(sizeof(PushConstants) == 128, "GroundPlane PushConstants must match smodel.vert");
        static_assert(offsetof(PushConstants, nodeInfo) == 96, "GroundPlane PushConstants::nodeInfo offset must match smodel.vert");

        struct CameraFrame
        {
            VkDescriptorSet set = VK_NULL_HANDLE;
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;

            VkBuffer paletteBuffer = VK_NULL_HANDLE;
            VkDeviceMemory paletteMemory = VK_NULL_HANDLE;
            void *paletteMapped = nullptr;
            uint32_t paletteCapacityMatrices = 0;

            VkBuffer jointPaletteBuffer = VK_NULL_HANDLE;
            VkDeviceMemory jointPaletteMemory = VK_NULL_HANDLE;
            void *jointPaletteMapped = nullptr;
            uint32_t jointPaletteCapacityMatrices = 0;
        };

        struct InstanceFrame
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void *mapped = nullptr;
        };

        struct PlaneVertex
        {
            glm::vec3 pos;
            glm::vec3 normal;
            glm::vec2 uv0;
            glm::vec4 tangent;

            glm::u16vec4 joints{0, 0, 0, 0};
            glm::vec4 weights{0.0f};
        };

    private:
        bool createCameraResources(VulkanContext &ctx, size_t frameCount);
        void destroyCameraResources();

        bool createInstanceResources(VulkanContext &ctx, size_t frameCount);
        void destroyInstanceResources();

        bool createMaterialResources(VulkanContext &ctx, size_t frameCount);
        void destroyMaterialResources();

        bool createGeometryResources(VulkanContext &ctx, size_t frameCount);
        void destroyGeometryResources();

        void updatePlaneForFrame(uint32_t frameIndex);

    private:
        bool m_enabled = true;
        AssetManager *m_assets = nullptr; // not owned
        Camera *m_camera = nullptr;       // not owned

        TextureHandle m_baseColorTexture{};

        float m_halfSize = 250.0f;
        float m_tileWorldSize = 5.0f;

        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkExtent2D m_extent{};

        // Camera descriptor set (set=0)
        VkDescriptorSetLayout m_cameraSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_cameraPool = VK_NULL_HANDLE;
        std::vector<CameraFrame> m_cameraFrames;

        // Material descriptor set (set=1)
        VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_materialPool = VK_NULL_HANDLE;
        std::vector<VkDescriptorSet> m_materialSets;

        // Per-frame instance buffer (binding=1)
        std::vector<InstanceFrame> m_instanceFrames;

        // Per-frame plane VB (binding=0)
        std::vector<VertexBufferHandle> m_planeVB;
        IndexBufferHandle m_planeIB{};

        Pipeline m_pipeline;

        PushConstants m_pc{};
    };
}
