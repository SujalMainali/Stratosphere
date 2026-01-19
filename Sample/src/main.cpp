#include "Engine/Application.h"
#include "Engine/TrianglesRenderPassModule.h"
#include "Engine/MeshRenderPassModule.h"
#include "Structs/SpawnGroup.h"
#include "assets/AssetManager.h"
#include "assets/MeshAsset.h"
#include "assets/Handles.h"
#include "assets/ModelAsset.h"
#include "assets/MaterialAsset.h"
#include "assets/TextureAsset.h"
#include "assets/MeshFormats.h"
#include "Engine/VulkanContext.h"
#include "Engine/Window.h"

#include "ECS/Prefab.h"
#include "ECS/PrefabSpawner.h"
#include "ECS/ECSContext.h"

#include "ScenarioSpawner.h"
#include "update.h"

#include "utils/BufferUtils.h" // CreateOrUpdateVertexBuffer + DestroyVertexBuffer
#include <iostream>
#include <filesystem>
#include <memory>
#include <random>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

class MySampleApp : public Engine::Application
{
public:
    MySampleApp() : Engine::Application()
    { // Create the AssetManager (uses Vulkan device & physical device)

        m_assets = std::make_unique<Engine::AssetManager>(
            GetVulkanContext().GetDevice(),
            GetVulkanContext().GetPhysicalDevice(),
            GetVulkanContext().GetGraphicsQueue(),
            GetVulkanContext().GetGraphicsQueueFamilyIndex());

        verifyLoadSModel();

        // Allow gameplay systems to resolve RenderMesh handles to loaded assets.
        m_systems.SetAssetManager(m_assets.get());

        setupECSFromPrefabs();

        // Systems can be initialized after prefabs are registered.
        m_systems.Initialize(GetECS().components);

        // Hook engine window events into our handler
        SetEventCallback([this](const std::string &e)
                         { this->OnEvent(e); });
    }

    ~MySampleApp() {}

    void Close() override
    {
        vkDeviceWaitIdle(GetVulkanContext().GetDevice());

        // Release model handle and collect unused assets
        m_assets->release(m_testModel);
        m_assets->garbageCollect();

        // Destroy triangle vertex buffer
        Engine::DestroyVertexBuffer(GetVulkanContext().GetDevice(), m_triangleVB);

        // Destroy triangle instance buffer
        Engine::DestroyVertexBuffer(GetVulkanContext().GetDevice(), m_triangleInstancesVB);

        // Release passes
        m_meshPass.reset();
        m_trianglesPass.reset();

        Engine::Application::Close();
    }

    void OnUpdate(Engine::TimeStep ts) override
    {
        m_systems.Update(GetECS(), ts.DeltaSeconds);
    }

    void OnRender() override
    {
        // Rendering handled by Renderer/Engine; no manual draw calls here
    }

private:
    static float pxToNdcX(double px, int w) { return static_cast<float>((px / double(w)) * 2.0 - 1.0); }
    static float pxToNdcY(double py, int h) { return static_cast<float>(((py / double(h)) * 2.0 - 1.0)); }

    void setupTriangleRenderer()
    // Legacy code:- Kept as a sample of minimal rendering process
    {
        // Interleaved vertex data: vec2 position, vec3 color (matches your triangle pipeline)
        const float vertices[] = {
            // x,     y,     r, g, b
            0.0f,
            -0.01f,
            1.0f,
            1.0f,
            1.0f,
            0.01f,
            0.01f,
            1.0f,
            1.0f,
            1.0f,
            -0.01f,
            0.01f,
            1.0f,
            1.0f,
            1.0f,
        };

        VkDevice device = GetVulkanContext().GetDevice();
        VkPhysicalDevice phys = GetVulkanContext().GetPhysicalDevice();

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
        m_triangleBinding.vertexCount = 3; // base triangle (instanced)
        m_trianglesPass->setVertexBinding(m_triangleBinding);

        // Create a placeholder instance buffer; we'll stream real ECS instances every frame.
        const float oneInstance[5] = {0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        VkDeviceSize instSize = sizeof(oneInstance);
        r = Engine::CreateOrUpdateVertexBuffer(device, phys, oneInstance, instSize, m_triangleInstancesVB);
        if (r != VK_SUCCESS)
            std::cerr << "Failed to create triangle instance buffer" << std::endl;

        Engine::TrianglesRenderPassModule::InstanceBinding inst{};
        inst.instanceBuffer = m_triangleInstancesVB.buffer;
        inst.offset = 0;
        inst.instanceCount = 1;
        m_trianglesPass->setInstanceBinding(inst);

        // Register pass to renderer
        GetRenderer().registerPass(m_trianglesPass);

        // Initial offset (push constants)
        m_trianglesPass->setOffset(0.0f, 0.0f);
    }

    void setupECSFromPrefabs()
    {
        auto &ecs = GetECS();

        // Load all prefab definitions from JSON copied next to executable.
        // (CMake copies Sample/entities/*.json -> <build>/Sample/entities/)
        size_t prefabCount = 0;
        try
        {
            for (const auto &entry : std::filesystem::directory_iterator("entities"))
            {
                if (!entry.is_regular_file())
                    continue;
                if (entry.path().extension() != ".json")
                    continue;
                const std::string path = entry.path().generic_string();
                const std::string jsonText = Engine::ECS::readFileText(path);
                if (jsonText.empty())
                {
                    std::cerr << "[Prefab] Failed to read: " << path << "\n";
                    continue;
                }
                Engine::ECS::Prefab p = Engine::ECS::loadPrefabFromJson(jsonText, ecs.components, ecs.archetypes, *m_assets);
                if (p.name.empty())
                {
                    std::cerr << "[Prefab] Missing name in: " << path << "\n";
                    continue;
                }
                ecs.prefabs.add(p);
                ++prefabCount;
                std::cout << "[Prefab] Loaded " << p.name << " from " << path << "\n";
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "[Prefab] Failed to enumerate entities/: " << e.what() << "\n";
            return;
        }

        if (prefabCount == 0)
        {
            std::cerr << "[Prefab] No prefabs loaded from entities/*.json\n";
            return;
        }

        Sample::SpawnFromScenarioFile(ecs, "Scinerio.json", /*selectSpawned=*/true);
    }

    void OnEvent(const std::string &name)
    {
        // Keep the mouse event wiring; logic will be updated later.
        if (name == "MouseMove" || name.rfind("MouseMove", 0) == 0)
        {
            auto &win = GetWindow();
            win.GetCursorPosition(m_lastMouseX, m_lastMouseY);
            return;
        }

        if (name == "MouseButtonLeftDown")
        {
            auto &win = GetWindow();
            win.GetCursorPosition(m_lastMouseX, m_lastMouseY);
            std::cout << "[Input] LeftDown px=(" << m_lastMouseX << "," << m_lastMouseY << ")\n";
            return;
        }

        if (name == "MouseButtonLeftUp")
        {
            auto &win = GetWindow();
            win.GetCursorPosition(m_lastMouseX, m_lastMouseY);
            std::cout << "[Input] LeftUp px=(" << m_lastMouseX << "," << m_lastMouseY << ")\n";
            return;
        }

        if (name == "MouseButtonRightDown")
        {
            auto &win = GetWindow();
            const int w = win.GetWidth();
            const int h = win.GetHeight();

            double mx = 0.0, my = 0.0;
            win.GetCursorPosition(mx, my);

            // Screen -> NDC (temporary; camera/world projection comes later)
            const float ndcX = pxToNdcX(mx, w);
            const float ndcY = pxToNdcY(my, h);
            std::cout << "[Input] RightDown px=(" << mx << "," << my << ") ndc=(" << ndcX << "," << ndcY << ")\n";
            return;
        }
    }
    void verifyLoadSModel()
    // Simple integrity test for .smodel loading
    {
        const char *modelPath = "assets/LuiteantHead/lieutenantHead.smodel";

        std::cout << "\n[SMODEL] Loading: " << modelPath << "\n";

        m_testModel = m_assets->loadModel(modelPath);
        if (!m_testModel.isValid())
        {
            std::cerr << "[SMODEL] loadModel failed: " << modelPath << "\n";
            return;
        }

        Engine::ModelAsset *model = m_assets->getModel(m_testModel);
        if (!model)
        {
            std::cerr << "[SMODEL] getModel returned nullptr\n";
            return;
        }

        std::cout << "[SMODEL] OK primitives=" << model->primitives.size() << "\n";

        if (model->primitives.empty())
        {
            std::cerr << "[SMODEL] Model has 0 primitives (unexpected)\n";
            return;
        }

        // Validate primitives -> mesh + material resolves
        for (size_t i = 0; i < model->primitives.size(); ++i)
        {
            const Engine::ModelPrimitive &prim = model->primitives[i];

            Engine::MeshAsset *mesh = m_assets->getMesh(prim.mesh);
            if (!mesh)
            {
                std::cerr << "[SMODEL] Primitive " << i << ": mesh resolve failed\n";
                continue;
            }

            std::cout << "  Prim[" << i << "] Mesh OK"
                      << " indices=" << prim.indexCount
                      << " vb=" << (void *)mesh->getVertexBuffer()
                      << " ib=" << (void *)mesh->getIndexBuffer()
                      << "\n";

            Engine::MaterialAsset *mat = m_assets->getMaterial(prim.material);
            if (!mat)
            {
                std::cout << "           Material: (missing)\n";
                continue;
            }

            std::cout << "           Material OK"
                      << " baseColor=("
                      << mat->baseColorFactor[0] << ", "
                      << mat->baseColorFactor[1] << ", "
                      << mat->baseColorFactor[2] << ", "
                      << mat->baseColorFactor[3] << ")\n";

            // Optional: verify textures resolve
            if (mat->baseColorTexture.isValid())
            {
                Engine::TextureAsset *tex = m_assets->getTexture(mat->baseColorTexture);
                std::cout << "           BaseColorTex: " << (tex ? "OK" : "FAILED") << "\n";
            }
            if (mat->normalTexture.isValid())
            {
                Engine::TextureAsset *tex = m_assets->getTexture(mat->normalTexture);
                std::cout << "           NormalTex: " << (tex ? "OK" : "FAILED") << "\n";
            }
        }

        std::cout << "[SMODEL] Verification complete \n\n";
    }

private:
    // Asset management
    std::unique_ptr<Engine::AssetManager> m_assets;

    // Triangle state
    Engine::VertexBufferHandle m_triangleVB{};
    Engine::VertexBufferHandle m_triangleInstancesVB{};
    std::shared_ptr<Engine::TrianglesRenderPassModule> m_trianglesPass;
    Engine::TrianglesRenderPassModule::VertexBinding m_triangleBinding{};
    double m_timeAccum = 0.0;

    // Mouse state
    double m_lastMouseX = 0.0;
    double m_lastMouseY = 0.0;

    // Mesh state
    std::shared_ptr<Engine::MeshRenderPassModule> m_meshPass;

    // Gameplay systems runner
    Sample::SystemRunner m_systems;

    Engine::ModelHandle m_testModel{};
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