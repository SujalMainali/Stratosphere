#include "Engine/Application.h"
#include "Engine/Window.h"
#include "Engine/VulkanContext.h"
#include "Engine/Renderer.h"
#include "Engine/SwapChain.h"
#include <iostream>
#include <chrono>

namespace Engine
{

    struct Application::Impl
    {
        std::unique_ptr<Window> window;
        std::unique_ptr<VulkanContext> vkContext;
        std::unique_ptr<Renderer> renderer;
        bool running = true;
        EventCallbackFn eventCallback;
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

            // Poll window events
            m_Impl->window->OnUpdate();

            // User update/render hooks
            TimeStep ts{};
            ts.DeltaSeconds = deltaSeconds;
            OnUpdate(ts);
            OnRender();

            // Draw one frame
            m_Impl->renderer->drawFrame();
        }

        // Orderly shutdown after main loop exits
        if (m_Impl->renderer)
        {
            try
            {
                m_Impl->renderer->cleanup();
            }
            catch (...)
            { /* avoid throw on shutdown */
            }
            m_Impl->renderer.reset();
        }
        if (m_Impl->vkContext)
        {
            // SwapChain cleanup is handled inside VulkanContext::Shutdown()
            m_Impl->vkContext->Shutdown();
            m_Impl->vkContext.reset();
        }
        // Window will be destroyed with unique_ptr reset/destructor
    }

    void Application::handleWindowEvent(const std::string &name)
    {
        if (m_Impl->eventCallback)
            m_Impl->eventCallback(name);
        if (name == "WindowClose" || name == "EscapePressed")
        {
            Close();
        }
        if (name == "WindowResize")
        {
            // Notify renderer that swapchain-dependent resources must be recreated
            if (m_Impl->renderer)
            {
                m_Impl->renderer->cleanup(); // Destroy renderer and all its resources
                VkExtent2D new_extent = {m_Impl->window->GetWidth(), m_Impl->window->GetHeight()};
                m_Impl->vkContext->GetSwapChain()->Recreate(new_extent);                // destory the swapchain and recreate with new extents                    // Recreate swapchain in context
                m_Impl->renderer->init(m_Impl->vkContext->GetSwapChain()->GetExtent()); // Re-initialize renderer resources
            }
        }
    }

    Window &Application::GetWindow() { return *m_Impl->window; }
    VulkanContext &Application::GetVulkanContext() { return *m_Impl->vkContext; }
    Renderer &Application::GetRenderer() { return *m_Impl->renderer; }

    void Application::Close()
    {
        // Signal loop exit first
        m_Impl->running = false;

        // Proactively clean up renderer while device/context are still alive
        if (m_Impl->renderer)
        {
            try
            {
                m_Impl->renderer->cleanup();
            }
            catch (...)
            {
            }
            // Keep context alive until Run() completes and calls Shutdown()
        }
    }

    void Application::SetEventCallback(const EventCallbackFn &callback)
    {
        m_Impl->eventCallback = callback;
    }

} // namespace Engine