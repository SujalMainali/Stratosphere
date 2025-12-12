#pragma once
#include "Structs/QueueFamilyStruct.h"
#include <vulkan/vulkan.h>
#include <optional>
#include <vector>
#include <memory>

namespace Engine
{
    class Window;
    class SwapChain; // Forward declaration of SwapChain
    class VulkanContext
    {
    public:
        VulkanContext(Window &window);
        ~VulkanContext();

        void Init();
        void Shutdown();
        void DrawFrame();

    private:
        void createInstance();
        void createSurface();
        void pickPhysicalDeviceForPresentation();
        QueueFamilyIndices findQueueFamiliesForPresentation(VkPhysicalDevice device) const;
        struct SwapChainSupportDetails
        {
            VkSurfaceCapabilitiesKHR capabilities{};
            std::vector<VkSurfaceFormatKHR> formats;
            std::vector<VkPresentModeKHR> presentModes;
        };
        SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) const;

        void createLogicalDevice();

    private:
        Window &m_Window;
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;

        struct SelectedDeviceInfo
        {
            VkPhysicalDevice physicalDevice;
            QueueFamilyIndices queueFamilyIndices;
        } m_SelectedDeviceInfo;

        VkDevice m_Device = VK_NULL_HANDLE;       // logical device
        VkQueue m_GraphicsQueue = VK_NULL_HANDLE; // graphics queue handle
        VkQueue m_PresentQueue = VK_NULL_HANDLE;

        std::unique_ptr<SwapChain> m_SwapChain;
    };

} // namespace Engine