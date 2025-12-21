#include <Engine/Application.h>
#include <Engine/Renderer.h>
#include <Engine/VulkanContext.h>
#include <Engine/TrianglesRenderPassModule.h>
#include <utils/BufferUtils.h>
#include <iostream>

class MySampleApp : public Engine::Application
{
public:
    MySampleApp()
    {
        // Ensure triangle resources are created and the pass is registered
        setupTriangle();

        SetEventCallback([this](const std::string &e)
                         {
                             const float step = 0.07f;
                             if (e == "LeftPressed")
                             {
                                m_offsetX = std::max(-1.0f, m_offsetX - step);
                                if (m_trianglesPass)
                                    m_trianglesPass->setOffset(m_offsetX, m_offsetY);
                             }
                             else if (e == "RightPressed")
                             {
                                m_offsetX = std::min(1.0f, m_offsetX + step);
                                if (m_trianglesPass)
                                    m_trianglesPass->setOffset(m_offsetX, m_offsetY);
                             } 
                            else if (e == "UpPressed")
                            {
                                m_offsetY = std::min(1.0f, m_offsetY - step);
                                if (m_trianglesPass)
                                    m_trianglesPass->setOffset(m_offsetX, m_offsetY);
                            }
                            else if (e == "DownPressed")
                            {
                                m_offsetY = std::max(-1.0f, m_offsetY + step);
                                if (m_trianglesPass)
                                    m_trianglesPass->setOffset(m_offsetX, m_offsetY);
                            } });
    }
    ~MySampleApp()
    {
    }

    // Destroy MySampleApp-owned Vulkan resources before device teardown
    void Close() override
    {
        // Wait for GPU to finish using resources before destroying them
        vkDeviceWaitIdle(GetVulkanContext().GetDevice());
        // Free vertex buffer while device is alive
        Engine::DestroyVertexBuffer(GetVulkanContext().GetDevice(), m_vertexBuffer);
        // Release triangles pass to trigger its pipeline/layout destruction
        m_trianglesPass.reset();
        // Now call base Close to stop loop and let renderer/context shut down
        Engine::Application::Close();
    }

    void OnUpdate(Engine::TimeStep ts) override
    {
        // game logic / ECS tick
        (void)ts;
    }

    void OnRender() override
    {
        // Rendering handled in Application::Run via renderer->drawFrame()
    }

private:
    void setupTriangle()
    {
        // Interleaved vertex data: vec2 position, vec3 color
        const float vertices[] = {
            // x, y,   r, g, b
            0.0f,
            -0.1f,
            1.0f,
            0.0f,
            0.0f,
            0.1f,
            0.1f,
            0.0f,
            1.0f,
            0.0f,
            -0.1f,
            0.1f,
            0.0f,
            0.0f,
            1.0f,
        };

        auto &ctx = GetVulkanContext();
        VkDevice device = ctx.GetDevice();
        VkPhysicalDevice phys = ctx.GetPhysicalDevice();

        // Create/upload vertex buffer
        VkDeviceSize dataSize = sizeof(vertices);
        VkResult r = Engine::CreateOrUpdateVertexBuffer(device, phys, vertices, dataSize, m_vertexBuffer);
        if (r != VK_SUCCESS)
        {
            std::cerr << "Failed to create vertex buffer" << std::endl;
            return;
        }

        // Create triangles pass and bind vertex buffer
        m_trianglesPass = std::make_shared<Engine::TrianglesRenderPassModule>();
        Engine::TrianglesRenderPassModule::VertexBinding binding{};
        binding.vertexBuffer = m_vertexBuffer.buffer;
        binding.offset = 0;
        binding.vertexCount = 3; // 3 vertices for one triangle
        m_trianglesPass->setVertexBinding(binding);

        // Register pass to renderer (renderer already initialized in Application ctor)
        GetRenderer().registerPass(m_trianglesPass);

        // Initialize offset (push constants) on the module
        m_trianglesPass->setOffset(m_offsetX, m_offsetY);
    }

    Engine::VertexBufferHandle m_vertexBuffer{};
    std::shared_ptr<Engine::TrianglesRenderPassModule> m_trianglesPass;
    float m_offsetX = 0.0f;
    float m_offsetY = 0.0f;
};

Engine::Application *Engine::CreateApplication()
{
    return new MySampleApp();
}

int main(int argc, char **argv)
{
    auto app = std::unique_ptr<Engine::Application>(Engine::CreateApplication());
    app->Run();
    return 0;
}