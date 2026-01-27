#include "Engine/ImGuiLayer.h"
#include "Engine/VulkanContext.h"
#include "Engine/Window.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>
#include <stdexcept>

namespace Engine
{
    ImGuiLayer::~ImGuiLayer()
    {
        cleanup();
    }

    bool ImGuiLayer::init(VulkanContext& ctx, Window& window, VkRenderPass renderPass, uint32_t imageCount)
    {
        if (m_initialized)
            return true;

        m_device = ctx.GetDevice();

        // Create descriptor pool for ImGui
        if (!createDescriptorPool(m_device))
        {
            return false;
        }

        // Setup Dear ImGui context
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

        // Setup style
        setupStyle();

        // Setup Platform/Renderer backends
        GLFWwindow* glfwWindow = static_cast<GLFWwindow*>(window.GetWindowPointer());
        ImGui_ImplGlfw_InitForVulkan(glfwWindow, true);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.Instance = ctx.GetInstance();
        initInfo.PhysicalDevice = ctx.GetPhysicalDevice();
        initInfo.Device = ctx.GetDevice();
        initInfo.QueueFamily = ctx.GetGraphicsQueueFamilyIndex();
        initInfo.Queue = ctx.GetGraphicsQueue();
        initInfo.PipelineCache = VK_NULL_HANDLE;
        initInfo.DescriptorPool = m_descriptorPool;
        initInfo.Subpass = 0;
        initInfo.MinImageCount = imageCount;
        initInfo.ImageCount = imageCount;
        initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        initInfo.Allocator = nullptr;
        initInfo.CheckVkResultFn = nullptr;

        ImGui_ImplVulkan_Init(&initInfo, renderPass);

        // Upload fonts
        ImGui_ImplVulkan_CreateFontsTexture();

        m_initialized = true;
        return true;
    }

    void ImGuiLayer::cleanup()
    {
        if (!m_initialized)
            return;

        vkDeviceWaitIdle(m_device);

        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (m_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);
            m_descriptorPool = VK_NULL_HANDLE;
        }

        m_initialized = false;
    }

    void ImGuiLayer::beginFrame()
    {
        if (!m_initialized)
            return;

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void ImGuiLayer::endFrame()
    {
        if (!m_initialized)
            return;

        // Call custom render callback
        if (m_renderCallback)
        {
            m_renderCallback();
        }

        ImGui::Render();
    }

    void ImGuiLayer::render(VkCommandBuffer cmd)
    {
        if (!m_initialized)
            return;

        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
    }

    void ImGuiLayer::onResize(uint32_t /*width*/, uint32_t /*height*/)
    {
        // ImGui handles resize automatically through GLFW callbacks
    }

    bool ImGuiLayer::createDescriptorPool(VkDevice device)
    {
        VkDescriptorPoolSize poolSizes[] = {
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
        };

        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        poolInfo.maxSets = 1000;
        poolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
        poolInfo.pPoolSizes = poolSizes;

        if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        {
            return false;
        }

        return true;
    }

    void ImGuiLayer::setupStyle()
    {
        ImGuiStyle& style = ImGui::GetStyle();

        // Modern dark theme with rounded corners
        style.WindowRounding = 8.0f;
        style.FrameRounding = 4.0f;
        style.GrabRounding = 4.0f;
        style.PopupRounding = 4.0f;
        style.ScrollbarRounding = 4.0f;
        style.TabRounding = 4.0f;

        style.WindowPadding = ImVec2(12.0f, 12.0f);
        style.FramePadding = ImVec2(8.0f, 4.0f);
        style.ItemSpacing = ImVec2(8.0f, 4.0f);
        style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);

        style.WindowBorderSize = 1.0f;
        style.FrameBorderSize = 0.0f;

        // Color scheme - dark blue/purple theme
        ImVec4* colors = style.Colors;
        colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.94f);
        colors[ImGuiCol_Border] = ImVec4(0.30f, 0.30f, 0.40f, 0.50f);
        colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
        colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.20f, 0.28f, 1.00f);
        colors[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.25f, 0.35f, 1.00f);
        colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
        colors[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.12f, 0.18f, 1.00f);
        colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.15f, 1.00f);
        colors[ImGuiCol_ScrollbarBg] = ImVec4(0.08f, 0.08f, 0.12f, 0.94f);
        colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.30f, 0.30f, 0.40f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.40f, 0.40f, 0.50f, 1.00f);
        colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.50f, 0.50f, 0.60f, 1.00f);
        colors[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.80f, 0.40f, 1.00f);
        colors[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.60f, 0.80f, 1.00f);
        colors[ImGuiCol_SliderGrabActive] = ImVec4(0.50f, 0.70f, 0.90f, 1.00f);
        colors[ImGuiCol_Button] = ImVec4(0.20f, 0.35f, 0.50f, 1.00f);
        colors[ImGuiCol_ButtonHovered] = ImVec4(0.25f, 0.45f, 0.60f, 1.00f);
        colors[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.55f, 0.70f, 1.00f);
        colors[ImGuiCol_Header] = ImVec4(0.20f, 0.35f, 0.50f, 0.80f);
        colors[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.45f, 0.60f, 0.80f);
        colors[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.55f, 0.70f, 1.00f);
        colors[ImGuiCol_Separator] = ImVec4(0.30f, 0.30f, 0.40f, 0.50f);
        colors[ImGuiCol_SeparatorHovered] = ImVec4(0.40f, 0.40f, 0.50f, 0.80f);
        colors[ImGuiCol_SeparatorActive] = ImVec4(0.50f, 0.50f, 0.60f, 1.00f);
        colors[ImGuiCol_Text] = ImVec4(0.90f, 0.90f, 0.95f, 1.00f);
        colors[ImGuiCol_TextDisabled] = ImVec4(0.50f, 0.50f, 0.55f, 1.00f);
        colors[ImGuiCol_PlotLines] = ImVec4(0.40f, 0.80f, 0.40f, 1.00f);
        colors[ImGuiCol_PlotHistogram] = ImVec4(0.40f, 0.60f, 0.80f, 1.00f);
    }

} // namespace Engine
