#pragma once
#include <memory>
#include <functional>
#include <string>

namespace Engine
{

    struct TimeStep
    {
        float DeltaSeconds = 0.0f;
    };

    class Window; // forward

    class Application
    {
    public:
        Application();
        virtual ~Application();

        // Start main loop (blocks)
        void Run();

        // Called by engine each frame; override in your Sample game class
        virtual void OnUpdate(TimeStep ts) {}

        // Optional: called after render submission, for UI, etc.
        virtual void OnRender() {}

        virtual void handleWindowEvent(const std::string &name);

        // Access to window
        Window &GetWindow();

        // Request application quit
        void Close();

        // Event callback dispatching (simple)
        using EventCallbackFn = std::function<void(const std::string &eventName)>;
        void SetEventCallback(const EventCallbackFn &callback);

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };

    Application *CreateApplication(); // implemented in Sample - returns user app
}