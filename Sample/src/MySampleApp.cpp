#include "MySampleApp.h"

#include "Engine/VulkanContext.h"
#include "Engine/Window.h"
#include "Engine/ImGuiLayer.h" 
#include <vulkan/vulkan.h>

#include "ECS/Prefab.h"
#include "ECS/PrefabSpawner.h"
#include "ECS/ECSContext.h"

#include "ScenarioSpawner.h"
#include "assets/AssetManager.h"

#include "Engine/GroundPlaneRenderPassModule.h"

#include <nlohmann/json.hpp>
#include <fstream>

#include <filesystem>
#include <iostream>
#include <limits>
#include <sstream>

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include "MenuManager.h"
#include <GLFW/glfw3.h>

using json = nlohmann::json;

MySampleApp::MySampleApp() : Engine::Application()
{
    m_assets = std::make_unique<Engine::AssetManager>(
        GetVulkanContext().GetDevice(),
        GetVulkanContext().GetPhysicalDevice(),
        GetVulkanContext().GetGraphicsQueue(),
        GetVulkanContext().GetGraphicsQueueFamilyIndex());


        m_menu.SetTextureLoader([this](const std::string& relpath) -> ImTextureID {
            if (!m_assets)
                return nullptr;

            // Load into an Engine texture asset
            Engine::TextureHandle th = m_assets->loadTextureFromFile(relpath);
            if (!th.isValid())
                return nullptr;

            Engine::TextureAsset* ta = m_assets->getTexture(th);
            if (!ta)
                return nullptr;

            VkSampler sampler = ta->getSampler();
            VkImageView view = ta->getView();
            if (sampler == VK_NULL_HANDLE || view == VK_NULL_HANDLE)
                return nullptr;

            // Register with ImGui (uses ImGui_ImplVulkan_AddTexture internally)
            Engine::ImGuiLayer* layer = GetImGuiLayer();
            if (!layer)
                return nullptr;

            return layer->addTexture(sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    // Optionally let the MenuManager know whether a save exists (so it can enable "Continue")
    m_menu.SetHasSaveFile(HasSaveFile());

    auto loader = m_menu.GetTextureLoader();
    if (loader)
    {
         // Use same relative path convention as MenuManager (example uses "assets/raw/...")
        ImTextureID bg   = loader("assets/raw/menu.png");
        ImTextureID tnew = loader("assets/raw/newgame.png");
        ImTextureID tcont= loader("assets/raw/continuegame.png");
        ImTextureID texit= loader("assets/raw/exit.png");
    }
    // RTS camera initialization (classic defaults).
    m_rtsCam.focus = {0.0f, 0.0f, 0.0f};
    m_rtsCam.yawDeg = -45.0f;
    m_rtsCam.pitchDeg = -55.0f;
    m_rtsCam.height = 70.0f;
    m_rtsCam.minHeight = 5.0f;
    m_rtsCam.maxHeight = 100.0f;

    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());
    ApplyRTSCamera(aspect);

    // Seed mouse position so the first frame doesn't produce a huge delta.
    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    m_lastMouse = {static_cast<float>(mx), static_cast<float>(my)};

    // Allow gameplay systems to resolve RenderModel handles to loaded assets.
    m_systems.SetAssetManager(m_assets.get());
    m_systems.SetRenderer(&GetRenderer());
    m_systems.SetCamera(&m_camera);

    // ------------------------------------------------------------
    // Background: simple ground-plane pass using ground baseColor tex
    // ------------------------------------------------------------
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

            // Keep texture alive even if the model/material are collected later.
            if (m_groundTexture.isValid())
                m_assets->addRef(m_groundTexture);

            // We only needed the texture; let the model be GC'd.
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

    // Systems can be initialized after prefabs are registered.
    m_systems.Initialize(GetECS().components);

    // Hook engine window events into our handler.
    SetEventCallback([this](const std::string &e)
                     { this->OnEvent(e); });

        // Detect whether a save exists so MenuManager can show Continue
    m_menu.SetHasSaveFile(HasSaveFile());

    // Hook up event callback (already present in file previously)
    SetEventCallback([this](const std::string &e)
                     { this->OnEvent(e); });
}

MySampleApp::~MySampleApp() = default;

void MySampleApp::Close()
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

void MySampleApp::OnUpdate(Engine::TimeStep ts)
{
    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());

    // Read mouse and compute per-frame delta.
    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);
    const glm::vec2 mouse{static_cast<float>(mx), static_cast<float>(my)};
    const glm::vec2 delta = mouse - m_lastMouse;
    m_lastMouse = mouse;

    // Pan (LMB drag) in ground plane; modifies focus only.
    if (m_isPanning)
    {
        if (m_panJustStarted)
        {
            // Prevent a jump on the initial press frame.
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
            // Update focus (not position). Mouse delta is in pixels.
            m_rtsCam.focus += (-rightXZ * delta.x + forwardXZ * delta.y) * panScale;
            m_rtsCam.focus.y = 0.0f;
        }
    }

    // Zoom (mouse wheel) modifies height.
    const float wheel = m_scrollDelta;
    m_scrollDelta = 0.0f;
    if (wheel != 0.0f)
    {
        m_rtsCam.height -= wheel * m_rtsCam.zoomSpeed;
        m_rtsCam.height = glm::clamp(m_rtsCam.height, m_rtsCam.minHeight, m_rtsCam.maxHeight);
    }

    // Apply RTS state to engine camera every frame.
    ApplyRTSCamera(aspect);

    m_systems.Update(GetECS(), ts.DeltaSeconds);
}

void MySampleApp::PickAndSelectEntityAtCursor()
{
    auto &ecs = GetECS();
    auto &win = GetWindow();

    double mx = 0.0, my = 0.0;
    win.GetCursorPosition(mx, my);

    const float mouseX = static_cast<float>(mx);
    const float mouseY = static_cast<float>(my);
    const float width = static_cast<float>(win.GetWidth());
    const float height = static_cast<float>(win.GetHeight());

    const uint32_t selectedId = ecs.components.ensureId("Selected");
    const uint32_t posId = ecs.components.ensureId("Position");
    const uint32_t rmId = ecs.components.ensureId("RenderModel");
    const uint32_t raId = ecs.components.ensureId("RenderAnimation");
    const uint32_t disabledId = ecs.components.ensureId("Disabled");
    const uint32_t deadId = ecs.components.ensureId("Dead");

    Engine::ECS::ComponentMask required;
    required.set(posId);
    required.set(rmId);
    required.set(raId);

    Engine::ECS::ComponentMask excluded;
    excluded.set(disabledId);
    excluded.set(deadId);

    // Clear existing selection first.
    for (const auto &ptr : ecs.stores.stores())
    {
        if (!ptr)
            continue;
        auto &store = *ptr;
        auto &masks = store.rowMasks();
        for (auto &mask : masks)
            mask.clear(selectedId);
    }

    // Project entities to screen; pick closest to cursor within a small radius.
    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 proj = m_camera.GetProjectionMatrix();
    const glm::mat4 vp = proj * view;

    constexpr float kPickRadiusPx = 50.0f;
    const float bestRadius2 = kPickRadiusPx * kPickRadiusPx;
    float bestD2 = bestRadius2;
    float bestCamD2 = std::numeric_limits<float>::infinity();

    Engine::ECS::ArchetypeStore *bestStore = nullptr;
    uint32_t bestRow = 0;

    for (const auto &ptr : ecs.stores.stores())
    {
        if (!ptr)
            continue;
        auto &store = *ptr;
        if (!store.signature().containsAll(required))
            continue;
        if (!store.signature().containsNone(excluded))
            continue;
        if (!store.hasPosition() || !store.hasRenderModel() || !store.hasRenderAnimation())
            continue;

        const auto &masks = store.rowMasks();
        const auto &positions = store.positions();
        const uint32_t n = store.size();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (!masks[row].matches(required, excluded))
                continue;

            const auto &p = positions[row];
            const glm::vec4 world(p.x, p.y, p.z, 1.0f);
            const glm::vec4 clip = vp * world;
            if (clip.w <= 1e-6f)
                continue;

            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f)
                continue;

            const float sx = (ndc.x * 0.5f + 0.5f) * width;
            // Camera projection already flips Y for Vulkan, so NDC Y is in the same "down is +" sense as window pixels.
            const float sy = (ndc.y * 0.5f + 0.5f) * height;

            const float dx = sx - mouseX;
            const float dy = sy - mouseY;
            const float d2 = dx * dx + dy * dy;

            const glm::vec3 camPos = m_camera.GetPosition();
            const glm::vec3 worldPos(p.x, p.y, p.z);
            const float camD2 = glm::dot(worldPos - camPos, worldPos - camPos);

            if (d2 < bestD2 || (std::abs(d2 - bestD2) < 1e-4f && camD2 < bestCamD2))
            {
                bestD2 = d2;
                bestCamD2 = camD2;
                bestStore = &store;
                bestRow = row;
            }
        }
    }

    if (!bestStore)
        return;

    // Apply selection and start animation.
    bestStore->rowMasks()[bestRow].set(selectedId);
    auto &anim = bestStore->renderAnimations()[bestRow];
    anim.playing = true;
    anim.timeSec = 0.0f;
}

void MySampleApp::ApplyRTSCamera(float aspect)
{
    // Projection stays perspective; keep it synced with window aspect.
    m_camera.SetPerspective(glm::radians(60.0f), aspect, 0.1f, 200.0f);

    // Direction from yaw/pitch.
    glm::vec3 forward;
    forward.x = std::cos(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward.y = std::sin(glm::radians(m_rtsCam.pitchDeg));
    forward.z = std::sin(glm::radians(m_rtsCam.yawDeg)) * std::cos(glm::radians(m_rtsCam.pitchDeg));
    forward = glm::normalize(forward);

    // Stable RTS mapping: keep a fixed slant while moving over ground.
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

void MySampleApp::OnRender()
{
    // Rendering handled by Renderer/Engine.
     // If ImGui frame active, draw the menu. (Application's Run begins an ImGui frame before OnRender.)
         // Apply fade-in effect to game world rendering
    if (m_menu.IsFadingToGame())
    {
        float alpha = m_menu.GetGameAlpha();
        // Apply alpha to your render pass or use a post-process overlay
        // Example: render a black quad with inverse alpha over everything
        // or multiply your fragment shader output by alpha
    }
    m_menu.OnImGuiFrame();

    // If menu produced a result, handle it
    if (m_menu.GetResult() != MenuManager::Result::None)
    {
        auto res = m_menu.GetResult();
        m_menu.ClearResult();

        if (res == MenuManager::Result::NewGame)
        {
            std::remove(m_saveFilePath.c_str());
            m_menu.SetHasSaveFile(false);
    
            // Start fade-in effect instead of just hiding
            m_menu.StartGameFadeIn();
    
            // optionally reset game state
        }
        else if (res == MenuManager::Result::ContinueGame)
        {
            // Load saved state (if present) and hide menu
            LoadGameState();
            m_menu.Hide();
        }
        else if (res == MenuManager::Result::Exit)
        {
            std::exit(0);  // Quick exit - no GPU wait, immediate termination
        }
    }
}

void MySampleApp::setupECSFromPrefabs()
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

void MySampleApp::OnEvent(const std::string &name)
{
    std::istringstream iss(name);
    std::string evt;
    iss >> evt;

    if (evt == "MouseButtonLeftDown")
    {
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

    if (evt == "MouseButtonRightDown")
    {
        PickAndSelectEntityAtCursor();
        return;
    }

    if (evt == "MouseScroll")
    {
        double xoff = 0.0, yoff = 0.0;
        iss >> xoff >> yoff;
        (void)xoff;
        m_scrollDelta += static_cast<float>(yoff);
        return;
    }

    if (name == "EscapePressed")
    {
        Close();
        return;
    }
}

void MySampleApp::SaveGameState()
{
    json j;
    j["rts_focus_x"] = m_rtsCam.focus.x;
    j["rts_focus_y"] = m_rtsCam.focus.y;
    j["rts_focus_z"] = m_rtsCam.focus.z;
    j["yawDeg"] = m_rtsCam.yawDeg;
    j["pitchDeg"] = m_rtsCam.pitchDeg;
    j["height"] = m_rtsCam.height;

    // Save window size using Engine::Window interface (available)
    j["win_w"] = static_cast<int>(GetWindow().GetWidth());
    j["win_h"] = static_cast<int>(GetWindow().GetHeight());

    // Save GLFW window position if available
    GLFWwindow* wnd = static_cast<GLFWwindow*>(GetWindow().GetWindowPointer());
    if (wnd)
    {
        int wx = 0, wy = 0;
        glfwGetWindowPos(wnd, &wx, &wy);
        j["win_x"] = wx;
        j["win_y"] = wy;
    }

    std::ofstream o(m_saveFilePath);
    if (o.good())
        o << j.dump(4);
}

void MySampleApp::LoadGameState()
{
    std::ifstream i(m_saveFilePath);
    if (!i.good()) return;
    json j;
    i >> j;

    m_rtsCam.focus.x = j.value("rts_focus_x", m_rtsCam.focus.x);
    m_rtsCam.focus.y = j.value("rts_focus_y", m_rtsCam.focus.y);
    m_rtsCam.focus.z = j.value("rts_focus_z", m_rtsCam.focus.z);
    m_rtsCam.yawDeg = j.value("yawDeg", m_rtsCam.yawDeg);
    m_rtsCam.pitchDeg = j.value("pitchDeg", m_rtsCam.pitchDeg);
    m_rtsCam.height = j.value("height", m_rtsCam.height);

    // Re-apply camera projection with current window aspect
    auto &win = GetWindow();
    const float aspect = static_cast<float>(win.GetWidth()) / static_cast<float>(win.GetHeight());
    ApplyRTSCamera(aspect);

    // Note: we intentionally do NOT call GLFW-specific functions to set window position/size here,
    // because the Engine::Window interface in this repo does not provide setters for position,
    // and directly depending on GLFW types in Sample sources caused the previous undefined-identifier issues.

    // Restore window position if saved (use GLFW directly via the window pointer)
    GLFWwindow* wnd = static_cast<GLFWwindow*>(GetWindow().GetWindowPointer());
    if (wnd)
    {
        int winx = j.value("win_x", INT32_MIN);
        int winy = j.value("win_y", INT32_MIN);
        if (winx != INT32_MIN && winy != INT32_MIN)
        {
            glfwSetWindowPos(wnd, winx, winy);
        }
    }
}


bool MySampleApp::HasSaveFile() const
{
    std::ifstream f(m_saveFilePath);
    return f.good();
}



// Expose the texture loader so callers can query it.