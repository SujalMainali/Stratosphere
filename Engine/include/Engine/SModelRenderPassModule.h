#pragma once
#include "Engine/Renderer.h"
#include "Engine/Pipeline.h"
#include "assets/AssetManager.h"
#include "assets/TextureAsset.h"
#include "Engine/Camera.h"
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>
#include <vector>
#include <unordered_map>

namespace Engine
{

    // RenderPassModule to render a cooked .smodel (ModelAsset) without node graph.
    // Draws all primitives at identity transform (or a caller-provided model matrix).
    class SModelRenderPassModule : public RenderPassModule
    {
    public:
        SModelRenderPassModule() = default;
        ~SModelRenderPassModule() override;

        const char *getDebugName() const override { return "SModel"; }

        void setEnabled(bool en) { m_enabled = en; }

        void setAssets(AssetManager *assets)
        {
            m_assets = assets;
            refreshModelMatrix();
        }
        void setModel(ModelHandle h)
        {
            m_model = h;
            refreshModelMatrix();
        }

        void setCamera(Camera *cam) { m_camera = cam; }

        // Slot-based instancing:
        // - Each visible entity gets a stable slot index while it remains visible.
        // - The per-frame draw call uses a dense active-slot list; the shader maps
        //   gl_InstanceIndex -> slotIndex -> instanceWorld/palettes.
        void setSlotLayout(uint32_t nodeCount, uint32_t jointCount);
        void ensureSlotCapacity(uint32_t slotCapacity);
        void setActiveSlots(const uint32_t *slotIndices, uint32_t count);

        void setSlotWorld(uint32_t slotIndex, const glm::mat4 &world);
        void setSlotPose(uint32_t slotIndex,
                 const glm::mat4 *nodeGlobals, uint32_t nodeCount,
                 const glm::mat4 *jointMatrices, uint32_t jointCount);

        // Column-major 4x4 matrix (16 floats). Defaults to identity.
        void setModelMatrix(const float *m16);

        void onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs) override;
        void record(FrameContext &frameCtx, VkCommandBuffer cmd) override;
        void onResize(VulkanContext &ctx, VkExtent2D newExtent) override;
        void onDestroy(VulkanContext &ctx) override;

    private:
        struct PushConstantsModel
        {
            float model[16];
            float baseColorFactor[4];
            float materialParams[4]; // x=alphaCutoff, y=alphaMode, z/w unused

            // Which node is being drawn; vertex shader fetches from palette[gl_InstanceIndex][nodeIndex]
            uint32_t nodeIndex = 0;
            uint32_t nodeCount = 0;

            // Pad to match GLSL uvec4 nodeInfo
            uint32_t _pad0 = 0;
            uint32_t _pad1 = 0;

            // Skinning info:
            // - skinBaseJoint: base offset into joint palette for this primitive's skin
            // - skinJointCount: number of joints in this skin (0 => unskinned)
            // - jointPaletteStride: total joint count for this model (used to stride per instance)
            // - flags: reserved
            uint32_t skinBaseJoint = 0;
            uint32_t skinJointCount = 0;
            uint32_t jointPaletteStride = 0;
            uint32_t flags = 0;
        };

        static_assert(sizeof(PushConstantsModel) == 128, "PushConstantsModel must match smodel.vert push constant block size");
        static_assert(offsetof(PushConstantsModel, nodeIndex) == 96, "PushConstantsModel::nodeIndex offset must match GLSL");

        struct CameraUBO
        {
            glm::mat4 view;
            glm::mat4 proj;
        };

        struct CameraFrame
        {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void *mapped = nullptr;
            VkDescriptorSet set = VK_NULL_HANDLE;

            // Tracks which active-slot list has been uploaded into this frame's SSBO.
            uint64_t lastUploadedActiveSlotsVersion = 0;

            VkBuffer paletteBuffer = VK_NULL_HANDLE;
            VkDeviceMemory paletteMemory = VK_NULL_HANDLE;
            void *paletteMapped = nullptr;
            uint32_t paletteCapacityMatrices = 0;

            VkBuffer jointPaletteBuffer = VK_NULL_HANDLE;
            VkDeviceMemory jointPaletteMemory = VK_NULL_HANDLE;
            void *jointPaletteMapped = nullptr;
            uint32_t jointPaletteCapacityMatrices = 0;

            VkBuffer instanceWorldBuffer = VK_NULL_HANDLE;
            VkDeviceMemory instanceWorldMemory = VK_NULL_HANDLE;
            void *instanceWorldMapped = nullptr;
            uint32_t instanceWorldCapacitySlots = 0;

            VkBuffer activeSlotsBuffer = VK_NULL_HANDLE;
            VkDeviceMemory activeSlotsMemory = VK_NULL_HANDLE;
            void *activeSlotsMapped = nullptr;
            uint32_t activeSlotsCapacity = 0;

            // Per-slot incremental upload tracking for this frame's buffers.
            std::vector<uint32_t> uploadedTransformEpoch;
            std::vector<uint32_t> uploadedPoseEpoch;
        };

        void destroyResources();
        void createPipelines(VulkanContext &ctx, VkRenderPass pass);
        bool refreshModelMatrix();
        bool createCameraResources(VulkanContext &ctx, size_t frameCount);
        void destroyCameraResources();
        bool ensurePaletteCapacity(CameraFrame &frame, uint32_t neededMatrices);
        bool ensureJointPaletteCapacity(CameraFrame &frame, uint32_t neededMatrices);

        bool ensureInstanceWorldCapacity(CameraFrame &frame, uint32_t neededSlots);
        bool ensureActiveSlotsCapacity(CameraFrame &frame, uint32_t needed);

        bool createMaterialResources(VulkanContext &ctx);
        void destroyMaterialResources();
        VkDescriptorSet getOrCreateMaterialSet(MaterialHandle h, const MaterialAsset *mat);

        VkPipelineColorBlendStateCreateInfo makeBlendState(bool enableBlend, VkPipelineColorBlendAttachmentState &outAttachment) const;

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
        VkExtent2D m_extent{};

        AssetManager *m_assets = nullptr;
        ModelHandle m_model{};
        Camera *m_camera = nullptr;

        bool m_enabled = true;

        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        Pipeline m_pipelineOpaque;
        Pipeline m_pipelineMask;
        Pipeline m_pipelineBlend;

        VkDescriptorSetLayout m_cameraSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_cameraPool = VK_NULL_HANDLE;
        std::vector<CameraFrame> m_cameraFrames;

        VkDescriptorSetLayout m_materialSetLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_materialPool = VK_NULL_HANDLE;
        std::unordered_map<uint64_t, VkDescriptorSet> m_materialSetCache;

        // Slot-based CPU-side data (indexed by stable slot index).
        uint32_t m_slotNodeCount = 0;
        uint32_t m_slotJointCount = 0;

        std::vector<uint32_t> m_activeSlots;
        uint64_t m_activeSlotsVersion = 1;

        std::vector<glm::mat4> m_slotWorlds;
        std::vector<uint32_t> m_slotTransformEpoch;
        uint32_t m_transformEpochCounter = 1;

        // Flattened node globals: [slot][node]
        std::vector<glm::mat4> m_nodePalette;
        // Flattened joint matrices: [slot][joint]
        std::vector<glm::mat4> m_jointPalette;
        std::vector<uint32_t> m_slotPoseEpoch;
        uint32_t m_poseEpochCounter = 1;

        TextureAsset m_fallbackWhiteTexture;

        PushConstantsModel m_pc{};
    };

} // namespace Engine
