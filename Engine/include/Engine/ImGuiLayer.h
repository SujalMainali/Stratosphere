#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <functional>

struct GLFWwindow;

namespace Engine
{
    class VulkanContext;
    class Window;

    /**
     * @brief ImGui integration layer for Vulkan rendering.
     * 
     * Handles ImGui initialization, font atlas creation, and per-frame rendering.
     * This is a simplified integration that renders ImGui as a post-process overlay.
     */
    class ImGuiLayer
    {
    public:
        ImGuiLayer() = default;
        ~ImGuiLayer();

        /**
         * @brief Initialize ImGui with Vulkan backend.
         * @param ctx Vulkan context
         * @param window GLFW window
         * @param renderPass The render pass to use for ImGui rendering
         * @param imageCount Number of swapchain images
         * @return true if initialization succeeded
         */
        bool init(VulkanContext& ctx, Window& window, VkRenderPass renderPass, uint32_t imageCount);

        /**
         * @brief Cleanup ImGui resources.
         */
        void cleanup();

        /**
         * @brief Begin a new ImGui frame. Call before any ImGui commands.
         */
        void beginFrame();

        /**
         * @brief End ImGui frame and prepare draw data. Call after all ImGui commands.
         */
        void endFrame();

        /**
         * @brief Record ImGui draw commands into the command buffer.
         * @param cmd Command buffer to record into
         */
        void render(VkCommandBuffer cmd);

        /**
         * @brief Handle window resize.
         * @param width New width
         * @param height New height
         */
        void onResize(uint32_t width, uint32_t height);

        /**
         * @brief Check if ImGui is initialized.
         */
        bool isInitialized() const { return m_initialized; }

        /**
         * @brief Set a callback to be invoked during ImGui frame for custom UI.
         */
        using RenderCallback = std::function<void()>;
        void setRenderCallback(RenderCallback callback) { m_renderCallback = callback; }

    private:
        bool createDescriptorPool(VkDevice device);
        void setupStyle();

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        bool m_initialized = false;
        RenderCallback m_renderCallback;
    };

} // namespace Engine
