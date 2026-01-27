#include "Engine/Window.h"
#include <GLFW/glfw3.h>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace Engine
{

    struct GLFWWindowData
    {
        GLFWwindow *Window = nullptr;
        Window::EventCallbackFn EventCallback;
        unsigned int Width = 1280, Height = 720;
        std::string Title;
    };

    class GLFWWindow : public Window
    {
    public:
        GLFWWindow(const WindowProps &props)
        {
            glfwSetErrorCallback([](int code, const char *desc)
                                 { fprintf(stderr, "[GLFW ERROR %d] %s\n", code, desc); });
            if (!glfwInit())
                throw std::runtime_error("GLFW init failed");
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // Vulkan
            data = new GLFWWindowData();
            data->Width = props.Width;
            data->Height = props.Height;
            data->Title = props.Title;
            data->Window = glfwCreateWindow(props.Width, props.Height, props.Title.c_str(), nullptr, nullptr);
            if (!data->Window)
            {
                glfwTerminate();
                throw std::runtime_error("Failed to create GLFW window");
            }
            glfwSetWindowUserPointer(data->Window, data);

            // Set GLFW callbacks
            glfwSetWindowCloseCallback(data->Window, [](GLFWwindow *wnd)
                                       {
                auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                if (d && d->EventCallback) {
                    d->EventCallback("WindowClose");
                } });

            glfwSetFramebufferSizeCallback(data->Window, [](GLFWwindow *wnd, int w, int h)
                                           {
                auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                if (!d) return;
                d->Width = static_cast<unsigned int>(w);
                d->Height = static_cast<unsigned int>(h);
                if (d->EventCallback) d->EventCallback("WindowResize"); });

            glfwSetKeyCallback(data->Window, [](GLFWwindow *wnd, int key, int /*scancode*/, int action, int /*mods*/)
                               {
                if (action == GLFW_PRESS || action == GLFW_REPEAT) {
                    auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                    if (!d || !d->EventCallback) return;
                    if (key == GLFW_KEY_LEFT)  d->EventCallback("LeftPressed");
                    if (key == GLFW_KEY_RIGHT) d->EventCallback("RightPressed");
                    if (key == GLFW_KEY_UP)    d->EventCallback("UpPressed");
                    if (key == GLFW_KEY_DOWN)  d->EventCallback("DownPressed");
                    if (key == GLFW_KEY_ESCAPE) d->EventCallback("EscapePressed");
                    if (key == GLFW_KEY_F1) d->EventCallback("F1Pressed");
                } });

            glfwSetCursorPosCallback(data->Window, [](GLFWwindow *wnd, double x, double y)
                                     {
                auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                if (!d || !d->EventCallback) return;
                std::ostringstream oss;
                oss << "MouseMove " << x << " " << y;
                d->EventCallback(oss.str()); });

            glfwSetMouseButtonCallback(data->Window, [](GLFWwindow *wnd, int button, int action, int /*mods*/)
                                       {
                if (action != GLFW_PRESS && action != GLFW_RELEASE) return;
                auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                if (!d || !d->EventCallback) return;

                const bool down = (action == GLFW_PRESS);
                if (button == GLFW_MOUSE_BUTTON_RIGHT) d->EventCallback(down ? "MouseButtonRightDown" : "MouseButtonRightUp");
                if (button == GLFW_MOUSE_BUTTON_LEFT)  d->EventCallback(down ? "MouseButtonLeftDown" : "MouseButtonLeftUp"); });

            glfwSetScrollCallback(data->Window, [](GLFWwindow *wnd, double xoff, double yoff)
                                  {
                auto d = static_cast<GLFWWindowData*>(glfwGetWindowUserPointer(wnd));
                if (!d || !d->EventCallback) return;
                std::ostringstream oss;
                oss << "MouseScroll " << xoff << " " << yoff;
                d->EventCallback(oss.str()); });
        }

        virtual ~GLFWWindow() override
        {
            if (data->Window)
                glfwDestroyWindow(data->Window);
            glfwTerminate();
            delete data;
        }

        virtual void OnUpdate() override
        {
            glfwPollEvents();
            // swap buffers only if you create an OpenGL context; for Vulkan, swap is in renderer
        }

        virtual unsigned int GetWidth() const override { return data->Width; }
        virtual unsigned int GetHeight() const override { return data->Height; }

        virtual void SetEventCallback(const EventCallbackFn &cb) override
        {
            data->EventCallback = cb; // This stores the function itself
        }

        virtual void *GetWindowPointer() override { return data->Window; }

        virtual void GetCursorPosition(double &x, double &y) const override
        {
            glfwGetCursorPos(data->Window, &x, &y);
        }

    private:
        GLFWWindowData *data;
    };

    std::unique_ptr<Window> Window::Create(const WindowProps &props)
    {
        return std::make_unique<GLFWWindow>(props);
    }

} // namespace Engine