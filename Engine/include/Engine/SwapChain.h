#pragma once
#include <vulkan/vulkan.h>
#include <vector>
#include "Structs/QueueFamilyStruct.h"

namespace Engine
{
    class SwapChain
    {
    public:
        // Construct with the Vulkan objects owned by VulkanContext.
        // The SwapChain will not create Vulkan objects until Init() is called.
        SwapChain(VkDevice device,
                  VkPhysicalDevice physicalDevice,
                  VkSurfaceKHR surface,
                  const Engine::QueueFamilyIndices &indices,
                  VkExtent2D initialExtent);
        ~SwapChain();

        // Initialize (create) the swapchain + image views.
        void Init();

        // Destroy owned Vulkan objects (image views, swapchain). Safe to call multiple times.
        void Cleanup();

        // Recreate the swapchain (e.g., on window resize). Caller should ensure device is idle or use fences.
        void Recreate(VkExtent2D newExtent);

        // Accessors
        VkSwapchainKHR GetSwapchain() const { return m_Swapchain; }
        const std::vector<VkImageView> &GetImageViews() const { return m_ImageViews; }
        VkFormat GetImageFormat() const { return m_ImageFormat; }
        VkExtent2D GetExtent() const { return m_Extent; }

    private:
        // internal helpers (similar to previous free functions)
        VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR> &available) const;
        VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR> &available) const;
        VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR &capabilities) const;
        void createImageViews();

    private:
        VkDevice m_Device;
        VkPhysicalDevice m_PhysicalDevice;
        VkSurfaceKHR m_Surface;
        Engine::QueueFamilyIndices m_QueueIndices;

        VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
        std::vector<VkImage> m_Images;
        std::vector<VkImageView> m_ImageViews;
        VkFormat m_ImageFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D m_Extent{};

        // Initial extent (window size) provided by VulkanContext when constructing
        VkExtent2D m_InitialExtent{};
    };

} // namespace Engine