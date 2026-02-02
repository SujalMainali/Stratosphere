#pragma once
#include <memory>
#include <functional>
#include <string>

namespace Engine
{
    class Window;
    class VulkanContext;
    class Renderer;
    class ImGuiLayer;

    struct TimeStep
    {
        float DeltaSeconds = 0.0f;
    };

    namespace ECS
    {
        struct ECSContext;
    }

    class Application
    {
    public:
        Application();
        virtual ~Application();

        // Start main loop (blocks)
        void Run();

        // Called by engine each frame; override in your Sample game class
        virtual void OnUpdate(TimeStep) {}

        // Optional: called after render submission, for UI, etc.
        virtual void OnRender() {}

        virtual void handleWindowEvent(const std::string &name);

        // Access to window
        Window &GetWindow();

        // New: access to VulkanContext and Renderer for sample setup
        VulkanContext &GetVulkanContext();
        Renderer &GetRenderer();
        // Access to ECS context (owned by Application)
        ECS::ECSContext &GetECS();

        // Request application quit
        virtual void Close();

        // Event callback dispatching (simple)
        using EventCallbackFn = std::function<void(const std::string &eventName)>;
        void SetEventCallback(const EventCallbackFn &callback);

        // Access to ImGuiLayer for texture registration with ImGui (optional)
        ImGuiLayer* GetImGuiLayer();

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    Application *CreateApplication(); // implemented in Sample - returns user app
}