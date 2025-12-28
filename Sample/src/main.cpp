#include "Engine/Application.h"
#include "Engine/VulkanContext.h"
#include "Engine/TrianglesRenderPassModule.h"
#include "Engine/MeshRenderPassModule.h"
#include "assets/MeshFormats.h"
#include "utils/BufferUtils.h" // CreateOrUpdateVertexBuffer + DestroyVertexBuffer

#include <algorithm>
#include <filesystem>
#include <iostream>

class MySampleApp : public Engine::Application
{
public:
    MySampleApp()
    {
        // Setup triangle (existing logic)
        setupTriangle();

        // Setup mesh from cooked asset
        setupMesh();
        // Start by showing the triangle, hide mesh
        m_showMesh = false;
        if (m_meshPass)
            m_meshPass->setEnabled(false);
        // Triangle: when hidden, set its vertexCount to 0 via binding
        // We'll keep triangle visible initially.
    }

    ~MySampleApp() {}

    void Close() override
    {
        // Wait for GPU to finish using resources before destroying them
        vkDeviceWaitIdle(GetVulkanContext().GetDevice());

        // Free mesh buffers
        Engine::DestroyVertexBuffer(GetVulkanContext().GetDevice(), m_meshVB);
        Engine::DestroyIndexBuffer(GetVulkanContext().GetDevice(), m_meshIB);

        // Free triangle vertex buffer
        Engine::DestroyVertexBuffer(GetVulkanContext().GetDevice(), m_triangleVB);

        // Release passes
        m_meshPass.reset();
        m_trianglesPass.reset();

        Engine::Application::Close();
    }

    void OnUpdate(Engine::TimeStep ts) override
    {
        // Alternate every 10 seconds
        m_timeAccum += ts.DeltaSeconds;
        if (m_timeAccum >= 10.0)
        {
            m_timeAccum -= 10.0;
            m_showMesh = !m_showMesh;

            // Toggle visibility
            if (m_meshPass)
                m_meshPass->setEnabled(m_showMesh);

            // Triangle pass: when hidden, set vertexCount to 0 via binding
            if (m_trianglesPass)
            {
                Engine::TrianglesRenderPassModule::VertexBinding binding = m_triangleBinding;
                binding.vertexCount = m_showMesh ? 0 : 3; // 3 verts when triangle visible, 0 to hide
                m_trianglesPass->setVertexBinding(binding);
            }
        }

        // Keyboard-driven offset control for triangle (existing sample behavior)
        // You may keep your event wiring; omitted here for brevity.
    }

    void OnRender() override
    {
        // Rendering handled in Application::Run via renderer->drawFrame()
    }

private:
    void setupTriangle()
    {
        // Interleaved vertex data: vec2 position, vec3 color (matches your triangle pipeline)
        const float vertices[] = {
            // x,    y,    r, g, b
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

        // Create/upload triangle vertex buffer
        VkDeviceSize dataSize = sizeof(vertices);
        VkResult r = Engine::CreateOrUpdateVertexBuffer(device, phys, vertices, dataSize, m_triangleVB);
        if (r != VK_SUCCESS)
        {
            std::cerr << "Failed to create triangle vertex buffer" << std::endl;
            return;
        }

        // Create triangles pass and bind vertex buffer
        m_trianglesPass = std::make_shared<Engine::TrianglesRenderPassModule>();
        m_triangleBinding.vertexBuffer = m_triangleVB.buffer;
        m_triangleBinding.offset = 0;
        m_triangleBinding.vertexCount = 3; // visible initially
        m_trianglesPass->setVertexBinding(m_triangleBinding);

        // Register pass to renderer
        GetRenderer().registerPass(m_trianglesPass);

        // Initial offset (push constants) on the module, if you have that UI logic
        m_trianglesPass->setOffset(0.0f, 0.0f);
    }

    void setupMesh()
    {
        // Load cooked mesh
        Engine::MeshData mesh;
        const std::string path = "assets/ObjModels/male.smesh";
        if (!Engine::LoadSMeshV0FromFile(path, mesh))
        {
            std::cerr << "Failed to load smesh: " << path
                      << " (cwd=" << std::filesystem::current_path().string() << ")" << std::endl;
            return;
        }

        auto &ctx = GetVulkanContext();
        VkDevice device = ctx.GetDevice();
        VkPhysicalDevice phys = ctx.GetPhysicalDevice();

        // Create GPU buffers
        VkResult rv = Engine::CreateOrUpdateVertexBuffer(device, phys,
                                                         mesh.vertexBytes.data(),
                                                         static_cast<VkDeviceSize>(mesh.vertexBytes.size()),
                                                         m_meshVB);
        if (rv != VK_SUCCESS)
        {
            std::cerr << "Failed to create mesh vertex buffer" << std::endl;
            return;
        }

        VkResult ri = VK_SUCCESS;
        VkIndexType indexType = VK_INDEX_TYPE_UINT32;
        if (mesh.indexFormat == 1)
        {
            ri = Engine::CreateOrUpdateIndexBuffer(device, phys,
                                                   mesh.indices32.data(),
                                                   static_cast<VkDeviceSize>(mesh.indices32.size() * sizeof(uint32_t)),
                                                   m_meshIB);
            indexType = VK_INDEX_TYPE_UINT32;
        }
        else
        {
            ri = Engine::CreateOrUpdateIndexBuffer(device, phys,
                                                   mesh.indices16.data(),
                                                   static_cast<VkDeviceSize>(mesh.indices16.size() * sizeof(uint16_t)),
                                                   m_meshIB);
            indexType = VK_INDEX_TYPE_UINT16;
        }
        if (ri != VK_SUCCESS)
        {
            std::cerr << "Failed to create mesh index buffer" << std::endl;
            return;
        }

        // Create & register mesh pass
        m_meshPass = std::make_shared<Engine::MeshRenderPassModule>();
        Engine::MeshRenderPassModule::MeshBinding binding{};
        binding.vertexBuffer = m_meshVB.buffer;
        binding.vertexOffset = 0;
        binding.indexBuffer = m_meshIB.buffer;
        binding.indexOffset = 0;
        binding.indexCount = mesh.indexCount;
        binding.indexType = indexType;
        m_meshPass->setMesh(binding);

        // Register to renderer (onCreate will run)
        GetRenderer().registerPass(m_meshPass);

        // Set extent (use renderer's swapchain extent)
        m_meshPass->onResize(GetVulkanContext(), GetRenderer().getExtent());
    }

private:
    // Triangle state
    Engine::VertexBufferHandle m_triangleVB{};
    std::shared_ptr<Engine::TrianglesRenderPassModule> m_trianglesPass;
    Engine::TrianglesRenderPassModule::VertexBinding m_triangleBinding{};
    float m_offsetX = 0.0f, m_offsetY = 0.0f;

    // Mesh state
    Engine::VertexBufferHandle m_meshVB{};
    Engine::IndexBufferHandle m_meshIB{};
    std::shared_ptr<Engine::MeshRenderPassModule> m_meshPass;

    // Toggle state
    bool m_showMesh = false;
    double m_timeAccum = 0.0;
};

int main()
{
    try
    {
        MySampleApp app;
        app.Run();
    }
    catch (const std::exception &e)
    {
        std::cerr << "Unhandled exception: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}