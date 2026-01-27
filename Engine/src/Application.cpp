#include "Engine/Application.h"
#include "Engine/Window.h"
#include "Engine/VulkanContext.h"
#include "Engine/Renderer.h"
#include "Engine/SwapChain.h"
#include "Engine/ImGuiLayer.h"
#include "Engine/PerformanceMonitor.h"
#include "ECS/ECSContext.h"
#include <iostream>
#include <chrono>

namespace Engine
{

    struct Application::Impl
    {
        std::unique_ptr<Window> window;
        std::unique_ptr<VulkanContext> vkContext;
        std::unique_ptr<Renderer> renderer;
        std::unique_ptr<ImGuiLayer> imguiLayer;
        std::unique_ptr<PerformanceMonitor> perfMonitor;
        bool running = true;
        EventCallbackFn eventCallback;
        std::unique_ptr<ECS::ECSContext> ecs;
    };

    Application::Application()
        : m_Impl(std::make_unique<Impl>())
    {
        // Create window (platform-specific implementation returns a concrete Window)
        m_Impl->window = Window::Create({"Engine Window", 1280, 720});

        m_Impl->window->SetEventCallback([this](const std::string &e)
                                         { this->handleWindowEvent(e); });

        // Create Vulkan context (owns instance, surface creation using the window handle)
        m_Impl->vkContext = std::make_unique<VulkanContext>(*m_Impl->window);
        m_Impl->renderer = std::make_unique<Renderer>(m_Impl->vkContext.get(), m_Impl->vkContext->GetSwapChain(), 4);

        // Initialize renderer now that swapchain exists
        m_Impl->renderer->init();

        // Initialize ImGui layer
        m_Impl->imguiLayer = std::make_unique<ImGuiLayer>();
        uint32_t imageCount = static_cast<uint32_t>(m_Impl->vkContext->GetSwapChain()->GetImageViews().size());
        m_Impl->imguiLayer->init(*m_Impl->vkContext, *m_Impl->window, 
                                  m_Impl->renderer->getMainRenderPass(), imageCount);

        // Initialize performance monitor
        m_Impl->perfMonitor = std::make_unique<PerformanceMonitor>();
        m_Impl->perfMonitor->init(m_Impl->vkContext.get(), m_Impl->renderer.get(), m_Impl->window.get());

        // Set up ImGui render callback to render performance overlay
        m_Impl->imguiLayer->setRenderCallback([this]() {
            if (m_Impl->perfMonitor) {
                m_Impl->perfMonitor->renderOverlay();
            }
        });

        // Set up renderer's ImGui render callback
        m_Impl->renderer->setImGuiRenderCallback([this](VkCommandBuffer cmd) {
            if (m_Impl->imguiLayer && m_Impl->imguiLayer->isInitialized()) {
                m_Impl->imguiLayer->render(cmd);
            }
        });

        m_Impl->ecs = std::make_unique<ECS::ECSContext>();
    }

    Application::~Application() = default;

    void Application::Run()
    {
        auto lastFrameTime = std::chrono::steady_clock::now();
        while (m_Impl->running)
        {
            const auto now = std::chrono::steady_clock::now();
            const float deltaSeconds = std::chrono::duration<float>(now - lastFrameTime).count();
            lastFrameTime = now;

            // Begin performance monitoring
            if (m_Impl->perfMonitor)
            {
                m_Impl->perfMonitor->beginFrame();
            }

            // Poll window events
            m_Impl->window->OnUpdate();

            // If a window event requested shutdown (Escape/WindowClose), stop cleanly
            // before running any further update/render work for this frame.
            if (!m_Impl->running)
                break;

            // Begin ImGui frame
            if (m_Impl->imguiLayer && m_Impl->imguiLayer->isInitialized())
            {
                m_Impl->imguiLayer->beginFrame();
            }

            // User update/render hooks
            TimeStep ts{};
            ts.DeltaSeconds = deltaSeconds;
            OnUpdate(ts);
            OnRender();

            // End ImGui frame (this also calls the render callback)
            if (m_Impl->imguiLayer && m_Impl->imguiLayer->isInitialized())
            {
                m_Impl->imguiLayer->endFrame();
            }

            // Draw one frame (includes ImGui rendering)
            m_Impl->renderer->drawFrame();

            // End performance monitoring
            if (m_Impl->perfMonitor)
            {
                m_Impl->perfMonitor->endFrame();
            }
        }
    }

    void Application::handleWindowEvent(const std::string &name)
    {
        if (m_Impl->eventCallback)
            m_Impl->eventCallback(name);
        if (name == "WindowClose" || name == "EscapePressed")
        {
            Close();
        }
        if (name == "F1Pressed")
        {
            // Toggle performance monitor overlay
            if (m_Impl->perfMonitor)
            {
                m_Impl->perfMonitor->toggle();
            }
        }
        if (name == "WindowResize")
        {
            // Notify renderer that swapchain-dependent resources must be recreated
            if (m_Impl->renderer)
            {
                // Cleanup ImGui before renderer cleanup
                if (m_Impl->imguiLayer)
                {
                    m_Impl->imguiLayer->cleanup();
                }

                m_Impl->renderer->cleanup(); // Destroy renderer and all its resources
                VkExtent2D new_extent = {m_Impl->window->GetWidth(), m_Impl->window->GetHeight()};
                m_Impl->vkContext->GetSwapChain()->Recreate(new_extent);                // destroy the swapchain and recreate with new extents
                m_Impl->renderer->init(m_Impl->vkContext->GetSwapChain()->GetExtent()); // Re-initialize renderer resources

                // Reinitialize ImGui with new render pass
                if (m_Impl->imguiLayer)
                {
                    uint32_t imageCount = static_cast<uint32_t>(m_Impl->vkContext->GetSwapChain()->GetImageViews().size());
                    m_Impl->imguiLayer->init(*m_Impl->vkContext, *m_Impl->window,
                                              m_Impl->renderer->getMainRenderPass(), imageCount);
                    
                    // Restore render callback
                    m_Impl->imguiLayer->setRenderCallback([this]() {
                        if (m_Impl->perfMonitor) {
                            m_Impl->perfMonitor->renderOverlay();
                        }
                    });

                    // Restore renderer's ImGui callback
                    m_Impl->renderer->setImGuiRenderCallback([this](VkCommandBuffer cmd) {
                        if (m_Impl->imguiLayer && m_Impl->imguiLayer->isInitialized()) {
                            m_Impl->imguiLayer->render(cmd);
                        }
                    });
                }

                if (m_Impl->imguiLayer)
                {
                    m_Impl->imguiLayer->onResize(new_extent.width, new_extent.height);
                }
            }
        }
    }

    Window &Application::GetWindow() { return *m_Impl->window; }
    VulkanContext &Application::GetVulkanContext() { return *m_Impl->vkContext; }
    Renderer &Application::GetRenderer() { return *m_Impl->renderer; }
    ECS::ECSContext &Application::GetECS() { return *m_Impl->ecs; }

    void Application::Close()
    {
        // Signal loop exit first
        m_Impl->running = false;
        
        // Cleanup ImGui
        if (m_Impl->imguiLayer)
        {
            m_Impl->imguiLayer->cleanup();
        }

        // Cleanup performance monitor
        if (m_Impl->perfMonitor)
        {
            m_Impl->perfMonitor->cleanup();
        }
    }

    void Application::SetEventCallback(const EventCallbackFn &callback)
    {
        m_Impl->eventCallback = callback;
    }

} // namespace Engine