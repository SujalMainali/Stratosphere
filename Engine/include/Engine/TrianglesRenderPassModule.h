#pragma once
#include "Engine/Renderer.h"
#include "Engine/Pipeline.h"
#include <vulkan/vulkan.h>
#include <vector>

namespace Engine
{

    // Simple module to draw N triangles (3*N vertices) from a vertex buffer.
    // Vertex format: location 0 = vec2 position, location 1 = vec3 color (optional).
    class TrianglesRenderPassModule : public RenderPassModule
    {
    public:
        struct VertexBinding
        {
            VkBuffer vertexBuffer = VK_NULL_HANDLE;
            VkDeviceSize offset = 0;
            uint32_t vertexCount = 0; // must be multiple of 3 for triangles
        };

        TrianglesRenderPassModule() = default;
        ~TrianglesRenderPassModule() override;

        // Provide/Update vertex buffer binding
        void setVertexBinding(const VertexBinding &binding);
        void setOffset(float x, float y)
        {
            m_offset[0] = x;
            m_offset[1] = y;
        }

        // RenderPassModule interface
        void onCreate(VulkanContext &ctx, VkRenderPass pass, const std::vector<VkFramebuffer> &fbs) override;
        void record(FrameContext &frameCtx, VkCommandBuffer cmd) override;
        void onResize(VulkanContext &ctx, VkExtent2D newExtent) override;
        void onDestroy(VulkanContext &ctx) override;

    private:
        void destroyResources();
        void createPipeline(VulkanContext &ctx, VkRenderPass pass);

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkExtent2D m_extent{};
        Pipeline m_pipeline;
        VertexBinding m_binding;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        float m_offset[2] = {0.0f, 0.0f};
    };

} // namespace Engine