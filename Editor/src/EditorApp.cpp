#include "EditorApp.h"

#include "Engine/VulkanContext.h"
#include "Engine/Window.h"
#include "Engine/ImGuiLayer.h"

#include "ECS/Prefab.h"
#include "ECS/PrefabSpawner.h"
#include "ECS/ECSContext.h"

#include "assets/AssetManager.h"
#include "Engine/GroundPlaneRenderPassModule.h"

#include "editor/GameWorldSpawner.h"

#include <nlohmann/json.hpp>

#include <imgui.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <sstream>

#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

using json = nlohmann::json;

EditorApp::EditorApp(std::string battleConfigPath)
    : Engine::Application(),
      m_battleConfigPath(std::move(battleConfigPath)),
#if defined(STRATO_EDITOR_ENTITIES_DIR)
      m_editorEntitiesDir(std::string(STRATO_EDITOR_ENTITIES_DIR)),
#else
      m_editorEntitiesDir(std::string("entities")),
#endif
      m_entityEditor(m_editorEntitiesDir)
{
    // Editor uses F1 for UI toggling; disable perf monitor/overlay entirely.
    SetPerformanceMonitorEnabled(false);

    m_assets = std::make_unique<Engine::AssetManager>(
        GetVulkanContext().GetDevice(),
        GetVulkanContext().GetPhysicalDevice(),
        GetVulkanContext().GetGraphicsQueue(),
        GetVulkanContext().GetGraphicsQueueFamilyIndex());

    // Basic RTS camera
    m_rtsCam.focus = {0.0f, 0.0f, 0.0f};
    m_rtsCam.yawDeg = -45.0f;
    m_rtsCam.pitchDeg = -55.0f;
    m_rtsCam.height = 120.0f;

    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());
    ApplyRTSCamera(aspect);

    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    m_lastMouse = {static_cast<float>(mx), static_cast<float>(my)};

    // Render-only runner (no gameplay simulation)
    m_render.SetAssetManager(m_assets.get());
    m_render.SetRenderer(&GetRenderer());
    m_render.SetCamera(&m_camera);

    // Optional ground plane (if assets exist)
    {
        Engine::ModelHandle groundModel = m_assets->loadModel("assets/Ground/scene.smodel");
        if (groundModel.isValid())
        {
            if (Engine::ModelAsset *m = m_assets->getModel(groundModel))
            {
                if (!m->primitives.empty())
                {
                    const Engine::MaterialHandle mh = m->primitives[0].material;
                    if (Engine::MaterialAsset *mat = m_assets->getMaterial(mh))
                    {
                        if (mat->baseColorTexture.isValid())
                            m_groundTexture = mat->baseColorTexture;
                    }
                }
            }

            if (m_groundTexture.isValid())
                m_assets->addRef(m_groundTexture);

            m_assets->release(groundModel);
        }

        if (m_groundTexture.isValid())
        {
            m_groundPass = std::make_shared<Engine::GroundPlaneRenderPassModule>();
            m_groundPass->setAssets(m_assets.get());
            m_groundPass->setCamera(&m_camera);
            m_groundPass->setBaseColorTexture(m_groundTexture);
            m_groundPass->setHalfSize(350.0f);
            m_groundPass->setTileWorldSize(5.0f);
            m_groundPass->setEnabled(true);
            GetRenderer().registerPass(m_groundPass);
        }
    }

    setupECSFromPrefabs();
    m_render.Initialize(GetECS());

    m_configEditor.loadFromFile(m_battleConfigPath);

    SetEventCallback([this](const std::string &e)
                     { this->OnEvent(e); });
}

EditorApp::~EditorApp() = default;

void EditorApp::Close()
{
    vkDeviceWaitIdle(GetVulkanContext().GetDevice());

    if (m_assets)
    {
        if (m_groundTexture.isValid())
            m_assets->release(m_groundTexture);
        m_assets->garbageCollect();
    }

    Engine::Application::Close();
}

void EditorApp::OnUpdate(Engine::TimeStep ts)
{
    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());

    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    const glm::vec2 mouse{static_cast<float>(mx), static_cast<float>(my)};
    const glm::vec2 delta = mouse - m_lastMouse;
    m_lastMouse = mouse;

    if (m_isPanning)
    {
        if (m_panJustStarted)
        {
            m_panJustStarted = false;
        }
        else
        {
            glm::vec3 forward;
            forward.x = std::cos(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
            forward.y = std::sin(glm::radians(m_rtsCam.pitchDeg));
            forward.z = std::sin(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
            forward = glm::normalize(forward);

            const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
            const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));

            glm::vec3 forwardXZ{forward.x, 0.0f, forward.z};
            glm::vec3 rightXZ{right.x, 0.0f, right.z};

            const float forwardLen2 = glm::dot(forwardXZ, forwardXZ);
            const float rightLen2 = glm::dot(rightXZ, rightXZ);
            if (forwardLen2 > 1e-6f)
                forwardXZ *= 1.0f / std::sqrt(forwardLen2);
            if (rightLen2 > 1e-6f)
                rightXZ *= 1.0f / std::sqrt(rightLen2);

            const float panScale = m_rtsCam.basePanSpeed * m_rtsCam.height;
            m_rtsCam.focus += (-rightXZ * delta.x + forwardXZ * delta.y) * panScale;
            m_rtsCam.focus.y = 0.0f;
        }
    }

    const float wheel = m_scrollDelta;
    m_scrollDelta = 0.0f;
    if (wheel != 0.0f)
    {
        m_rtsCam.height -= wheel * m_rtsCam.zoomSpeed;
        m_rtsCam.height = glm::clamp(m_rtsCam.height, m_rtsCam.minHeight, m_rtsCam.maxHeight);
    }

    ApplyRTSCamera(aspect);

    // Keep render-related data updated, but do not run gameplay systems.
    (void)ts;
    m_render.Update(GetECS(), 0.0f);
}

void EditorApp::OnRender()
{
    Engine::ImGuiLayer *layer = GetImGuiLayer();
    if (!layer || !layer->isInitialized() || ImGui::GetCurrentContext() == nullptr)
        return;

    auto &ecs = GetECS();
    m_configEditor.draw(ecs);

    if (!m_showEntityEditor)
        return;

    ImGuiIO &io = ImGui::GetIO();
    const float panelWidth = 520.0f;
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.78f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Editor: Entity Types", nullptr, flags);

    Editor::EntityTypeEditor::Callbacks cb;
    cb.reloadPrefabsAndRespawn = [](void *user)
    {
        auto *self = static_cast<EditorApp *>(user);
        self->reloadPrefabsAndRespawnWorld();
    };
    cb.user = this;

    m_entityEditor.draw(ecs, *m_assets, cb);

    ImGui::End();
}

void EditorApp::setupECSFromPrefabs()
{
    auto &ecs = GetECS();

    ecs.prefabs.clear();
    loadPrefabsFromDir("entities");
    loadPrefabsFromDir(m_editorEntitiesDir);

    Editor::SpawnFromGameWorldFile(ecs, m_battleConfigPath, /*selectSpawned=*/false);
}

void EditorApp::loadPrefabsFromDir(const std::string &dirPath)
{
    auto &ecs = GetECS();
    try
    {
        if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath))
            return;

        for (const auto &entry : std::filesystem::directory_iterator(dirPath))
        {
            if (!entry.is_regular_file())
                continue;
            if (entry.path().extension() != ".json")
                continue;

            const std::string path = entry.path().generic_string();
            const std::string jsonText = Engine::ECS::readFileText(path);
            if (jsonText.empty())
                continue;

            Engine::ECS::Prefab p = Engine::ECS::loadPrefabFromJson(jsonText, ecs.components, ecs.archetypes, *m_assets);
            if (p.name.empty())
                continue;

            ecs.prefabs.add(p);
        }
    }
    catch (...)
    {
    }
}

void EditorApp::clearAllEntities()
{
    auto &ecs = GetECS();
    for (const auto &storePtr : ecs.stores.stores())
    {
        if (!storePtr)
            continue;

        while (storePtr->size() > 0)
        {
            Engine::ECS::Entity e = storePtr->entities()[storePtr->size() - 1];
            ecs.entities.destroy(e);
            storePtr->destroyRow(storePtr->size() - 1);
        }
    }
}

void EditorApp::reloadPrefabsAndRespawnWorld()
{
    auto &ecs = GetECS();
    clearAllEntities();

    ecs.prefabs.clear();
    loadPrefabsFromDir("entities");
    loadPrefabsFromDir(m_editorEntitiesDir);

    Editor::SpawnFromGameWorldFile(ecs, m_battleConfigPath, /*selectSpawned=*/false);
}

void EditorApp::OnEvent(const std::string &name)
{
    std::istringstream iss(name);
    std::string evt;
    iss >> evt;

    if (evt == "MouseButtonLeftDown")
    {
        if (ImGui::GetIO().WantCaptureMouse)
            return;

        m_isPanning = true;
        m_panJustStarted = true;
        auto &win = GetWindow();
        double mx = 0.0, my = 0.0;
        win.GetCursorPosition(mx, my);
        m_lastMouse = {static_cast<float>(mx), static_cast<float>(my)};
        return;
    }

    if (evt == "MouseButtonLeftUp")
    {
        m_isPanning = false;
        m_panJustStarted = false;
        return;
    }

    if (evt == "MouseScroll")
    {
        if (ImGui::GetIO().WantCaptureMouse)
            return;

        double xoff = 0.0, yoff = 0.0;
        iss >> xoff >> yoff;
        (void)xoff;
        m_scrollDelta += static_cast<float>(yoff);
        return;
    }

    if (name == "F2Pressed")
    {
        m_configEditor.toggleVisible();
        return;
    }

    if (name == "F1Pressed")
    {
        m_showEntityEditor = !m_showEntityEditor;
        return;
    }

    if (name == "WindowResize")
        return;
}

void EditorApp::ApplyRTSCamera(float aspect)
{
    m_camera.SetPerspective(45.0f, aspect, 0.1f, 2000.0f);

    glm::vec3 forward;
    forward.x = std::cos(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward.y = std::sin(glm::radians(m_rtsCam.pitchDeg));
    forward.z = std::sin(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward = glm::normalize(forward);

    glm::vec3 forwardXZ{forward.x, 0.0f, forward.z};
    const float forwardLen2 = glm::dot(forwardXZ, forwardXZ);
    if (forwardLen2 > 1e-6f)
        forwardXZ *= 1.0f / std::sqrt(forwardLen2);
    else
        forwardXZ = {0.0f, 0.0f, -1.0f};

    const float backDistance = m_rtsCam.height;
    const glm::vec3 camPos = m_rtsCam.focus - forwardXZ * backDistance + glm::vec3(0.0f, m_rtsCam.height, 0.0f);

    m_camera.SetPosition(camPos);
    m_camera.SetRotation(m_rtsCam.yawDeg, m_rtsCam.pitchDeg);
}
