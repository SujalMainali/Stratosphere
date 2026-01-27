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

        VkPhysicalDevice GetPhysicalDevice() const { return m_SelectedDeviceInfo.physicalDevice; }
        VkDevice GetDevice() const { return m_Device; }
        VkQueue GetGraphicsQueue() const { return m_GraphicsQueue; }
        VkQueue GetPresentQueue() const { return m_PresentQueue; }
        VkInstance GetInstance() const { return m_Instance; }
        uint32_t GetGraphicsQueueFamilyIndex() const { return m_SelectedDeviceInfo.queueFamilyIndices.graphicsFamily.value(); }
        SwapChain *GetSwapChain() const { return m_SwapChain.get(); }

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

        bool checkValidationLayerSupport();
        void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT &createInfo);

    private:
        Window &m_Window;
        VkInstance m_Instance = VK_NULL_HANDLE;
        VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;

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