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
#include <algorithm>

#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

using json = nlohmann::json;

// -----------------------------------------------------------------------------
// TUNABLES (ALL_CAPS)
// -----------------------------------------------------------------------------
namespace SampleTuning
{
    // Cursor picking: screen-space radius to consider a unit under the cursor.
    static constexpr float SELECTION_PICK_RADIUS_PX = 30.0f;

    // Selection: local cluster (connected component) radius.
    static constexpr float SELECTION_LINK_RADIUS_M = 14.0f;
    static constexpr uint32_t SELECTION_MAX_UNITS = 2048;

    // Enemy-click behavior: if nothing is selected, pick a friendly seed near the enemy.
    static constexpr float ENEMY_CLICK_SEED_SEARCH_RADIUS_M = 80.0f;

    // HUD grouping: cluster by (team + proximity) using spatial index.
    static constexpr float HUD_GROUP_LINK_RADIUS_M = 20.0f;

    // HUD: bar dimensions in pixels.
    static constexpr float HUD_BAR_WIDTH_PX = 70.0f;
    static constexpr float HUD_BAR_HEIGHT_PX = 7.0f;
    static constexpr float HUD_BAR_CORNER_ROUNDING_PX = 3.0f;
    static constexpr float HUD_BAR_BORDER_THICKNESS_PX = 1.0f;
    static constexpr float HUD_TEXT_OFFSET_Y_PX = 30.0f;

    // HUD: world-space height above group centroid.
    static constexpr float HUD_WORLD_HEIGHT_OFFSET_M = 10.0f;

    // HUD: colors (RGBA). Kept as constants so opacity is easy to tweak.
    static constexpr ImU32 HUD_TEAM0_FILL = IM_COL32(50, 120, 220, 170);
    static constexpr ImU32 HUD_TEAM1_FILL = IM_COL32(200, 50, 50, 170);
    static constexpr ImU32 HUD_TEAM0_BG = IM_COL32(20, 40, 80, 90);
    static constexpr ImU32 HUD_TEAM1_BG = IM_COL32(80, 20, 20, 90);
    static constexpr ImU32 HUD_BORDER = IM_COL32(220, 220, 220, 90);
    static constexpr ImU32 HUD_TEXT = IM_COL32(235, 235, 235, 190);
}

MySampleApp::MySampleApp() : Engine::Application()
{
    m_assets = std::make_unique<Engine::AssetManager>(
        GetVulkanContext().GetDevice(),
        GetVulkanContext().GetPhysicalDevice(),
        GetVulkanContext().GetGraphicsQueue(),
        GetVulkanContext().GetGraphicsQueueFamilyIndex());

    // RTS camera initialization — raised for the larger maze map.
    m_rtsCam.focus = {0.0f, 0.0f, 0.0f};
    m_rtsCam.yawDeg = -45.0f;
    m_rtsCam.pitchDeg = -55.0f;
    m_rtsCam.height = 120.0f;
    m_rtsCam.minHeight = 5.0f;
    m_rtsCam.maxHeight = 250.0f;

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
            m_groundPass->setHalfSize(1000.0f);
            m_groundPass->setTileWorldSize(5.0f);
            m_groundPass->setEnabled(true);
            GetRenderer().registerPass(m_groundPass);
        }
    }

    setupECSFromPrefabs();

    // Enable incremental query updates when new stores are created.
    GetECS().WireQueryManager();

    // Systems can be initialized after prefabs are registered.
    m_systems.Initialize(GetECS());

    // Hook engine window events into our handler.
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
    const uint32_t teamId = ecs.components.ensureId("Team");
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

    // Project entities to screen; pick closest to cursor within a small radius.
    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 proj = m_camera.GetProjectionMatrix();
    const glm::mat4 vp = proj * view;

    const float bestRadius2 = SampleTuning::SELECTION_PICK_RADIUS_PX * SampleTuning::SELECTION_PICK_RADIUS_PX;
    float bestD2 = bestRadius2;
    float bestCamD2 = std::numeric_limits<float>::infinity();

    Engine::ECS::ArchetypeStore *bestStore = nullptr;
    uint32_t bestRow = 0;

    static Engine::ECS::QueryId pickQueryId = Engine::ECS::QueryManager::InvalidQuery;
    if (pickQueryId == Engine::ECS::QueryManager::InvalidQuery)
        pickQueryId = ecs.queries.createQuery(required, excluded, ecs.stores);

    const auto &q = ecs.queries.get(pickQueryId);
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
        if (!storePtr)
            continue;
        auto &store = *storePtr;
        if (!store.hasPosition() || !store.hasRenderModel() || !store.hasRenderAnimation())
            continue;

        const auto &positions = store.positions();
        const uint32_t n = store.size();
        for (uint32_t row = 0; row < n; ++row)
        {
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

    if (bestStore)
    {
        // Clicked on an entity.
        // - Friendly: select the entire group (all living units with the same Team id).
        // - Enemy: issue an attack-move for currently-selected units toward that enemy.

        const Engine::ECS::Entity picked = bestStore->entities()[bestRow];
        const auto &pickedPos = bestStore->positions()[bestRow];

        const int playerTeam = m_systems.GetCombatSystem().humanTeamId();
        const bool havePickedTeam = bestStore->hasTeam();
        const uint8_t pickedTeam = havePickedTeam ? bestStore->teams()[bestRow].id : 0u;

        auto clearSelection = [&]()
        {
            std::vector<Engine::ECS::Entity> toClear;
            toClear.reserve(256);
            for (const auto &ptr : ecs.stores.stores())
            {
                if (!ptr)
                    continue;
                const auto &store = *ptr;
                if (!store.signature().has(selectedId))
                    continue;
                const auto &ents = store.entities();
                const uint32_t n = store.size();
                for (uint32_t row = 0; row < n; ++row)
                    toClear.push_back(ents[row]);
            }
            for (const auto &e : toClear)
                (void)ecs.removeTag(e, selectedId);
        };

        auto hasAnySelected = [&]() -> bool
        {
            for (const auto &ptr : ecs.stores.stores())
            {
                if (!ptr)
                    continue;
                const auto &store = *ptr;
                if (store.signature().has(selectedId) && store.size() > 0)
                    return true;
            }
            return false;
        };

        auto selectLocalClusterForTeam = [&](uint8_t teamToSelect, const Engine::ECS::Entity &seed)
        {
            clearSelection();

            // Flood-fill (connected component) selection: starting from the seed,
            // keep adding same-team units that are within a link radius of any selected unit.
            const float linkR2 = SampleTuning::SELECTION_LINK_RADIUS_M * SampleTuning::SELECTION_LINK_RADIUS_M;
            constexpr uint32_t kMaxSelect = SampleTuning::SELECTION_MAX_UNITS;

            auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
            {
                return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
            };

            std::vector<Engine::ECS::Entity> queue;
            queue.reserve(256);
            std::vector<Engine::ECS::Entity> selected;
            selected.reserve(512);

            std::unordered_set<uint64_t> visited;
            visited.reserve(512);

            queue.push_back(seed);
            visited.insert(entityKey(seed));

            const auto &spatial = m_systems.GetSpatialIndex();

            while (!queue.empty() && selected.size() < kMaxSelect)
            {
                const Engine::ECS::Entity cur = queue.back();
                queue.pop_back();

                auto *rec = ecs.entities.find(cur);
                if (!rec)
                    continue;
                auto *st = ecs.stores.get(rec->archetypeId);
                if (!st || rec->row >= st->size())
                    continue;
                if (!st->hasPosition() || !st->hasTeam() || !st->hasHealth())
                    continue;
                if (st->healths()[rec->row].value <= 0.0f)
                    continue;
                if (st->teams()[rec->row].id != teamToSelect)
                    continue;

                // Accept this entity into the selection.
                selected.push_back(cur);

                const float cx = st->positions()[rec->row].x;
                const float cz = st->positions()[rec->row].z;

                spatial.forNeighborsInRadius(cx, cz, SampleTuning::SELECTION_LINK_RADIUS_M, [&](uint32_t nStoreId, uint32_t nRow)
                                             {
                                                 auto *ns = ecs.stores.get(nStoreId);
                                                 if (!ns)
                                                     return;
                                                 if (nRow >= ns->size())
                                                     return;
                                                 if (!ns->hasPosition() || !ns->hasTeam() || !ns->hasHealth())
                                                     return;
                                                 if (ns->signature().has(disabledId) || ns->signature().has(deadId))
                                                     return;
                                                 if (ns->healths()[nRow].value <= 0.0f)
                                                     return;
                                                 if (ns->teams()[nRow].id != teamToSelect)
                                                     return;

                                                 const float dx = ns->positions()[nRow].x - cx;
                                                 const float dz = ns->positions()[nRow].z - cz;
                                                 if (dx * dx + dz * dz > linkR2)
                                                     return;

                                                 const Engine::ECS::Entity cand = ns->entities()[nRow];
                                                 const uint64_t k = entityKey(cand);
                                                 if (visited.insert(k).second)
                                                     queue.push_back(cand); });
            }

            for (const auto &e : selected)
                (void)ecs.addTag(e, selectedId);
        };

        const bool isEnemy = (playerTeam >= 0 && havePickedTeam && pickedTeam != static_cast<uint8_t>(playerTeam));
        if (!isEnemy)
        {
            // Friendly group selection (local cluster)
            selectLocalClusterForTeam(pickedTeam, picked);
        }
        else
        {
            // Enemy clicked: keep current selection if any; otherwise select a nearby friendly cluster.
            if (playerTeam >= 0 && !hasAnySelected())
            {
                const uint8_t teamToSelect = static_cast<uint8_t>(playerTeam);

                // Find the closest friendly unit near the clicked enemy as a seed.
                Engine::ECS::Entity bestSeed{};
                float bestD2 = std::numeric_limits<float>::infinity();

                const float sx = pickedPos.x;
                const float sz = pickedPos.z;

                const auto &spatial = m_systems.GetSpatialIndex();
                spatial.forNeighborsInRadius(sx, sz, SampleTuning::ENEMY_CLICK_SEED_SEARCH_RADIUS_M, [&](uint32_t nStoreId, uint32_t nRow)
                                             {
                                                 auto *ns = ecs.stores.get(nStoreId);
                                                 if (!ns)
                                                     return;
                                                 if (nRow >= ns->size())
                                                     return;
                                                 if (!ns->hasPosition() || !ns->hasTeam() || !ns->hasHealth())
                                                     return;
                                                 if (ns->signature().has(disabledId) || ns->signature().has(deadId))
                                                     return;
                                                 if (ns->healths()[nRow].value <= 0.0f)
                                                     return;
                                                 if (ns->teams()[nRow].id != teamToSelect)
                                                     return;

                                                 const float dx = ns->positions()[nRow].x - sx;
                                                 const float dz = ns->positions()[nRow].z - sz;
                                                 const float d2 = dx * dx + dz * dz;
                                                 if (d2 < bestD2)
                                                 {
                                                     bestD2 = d2;
                                                     bestSeed = ns->entities()[nRow];
                                                 } });

                if (bestSeed.valid())
                    selectLocalClusterForTeam(teamToSelect, bestSeed);
            }

            // Use existing command pipeline (formation offsets) toward the enemy position.
            m_systems.SetGlobalMoveTarget(pickedPos.x, 0.0f, pickedPos.z);
        }
    }
    else
    {
        // Clicked on ground - move selected units to this position
        // Ray-cast from camera through cursor to ground plane (Y=0)

        // Convert mouse coords to NDC
        const float ndcX = (mouseX / width) * 2.0f - 1.0f;
        const float ndcY = (mouseY / height) * 2.0f - 1.0f;

        // Unproject near and far points
        const glm::mat4 invVP = glm::inverse(vp);
        const glm::vec4 nearClip(ndcX, ndcY, 0.0f, 1.0f); // Near plane (z=0 in NDC)
        const glm::vec4 farClip(ndcX, ndcY, 1.0f, 1.0f);  // Far plane (z=1 in NDC)

        glm::vec4 nearWorld = invVP * nearClip;
        glm::vec4 farWorld = invVP * farClip;

        if (std::abs(nearWorld.w) > 1e-6f)
            nearWorld /= nearWorld.w;
        if (std::abs(farWorld.w) > 1e-6f)
            farWorld /= farWorld.w;

        const glm::vec3 rayOrigin(nearWorld);
        const glm::vec3 rayDir = glm::normalize(glm::vec3(farWorld) - glm::vec3(nearWorld));

        // Intersect with ground plane (Y = 0)
        // Ray: P = O + t * D
        // Plane: Y = 0  =>  O.y + t * D.y = 0  =>  t = -O.y / D.y
        if (std::abs(rayDir.y) > 1e-6f)
        {
            const float t = -rayOrigin.y / rayDir.y;
            if (t > 0.0f)
            {
                const glm::vec3 hitPoint = rayOrigin + t * rayDir;

                // Send move command to selected units
                m_systems.SetGlobalMoveTarget(hitPoint.x, 0.0f, hitPoint.z);

                (void)hitPoint;
            }
        }
    }
}

void MySampleApp::ApplyRTSCamera(float aspect)
{
    // Projection stays perspective; keep it synced with window aspect.
    m_camera.SetPerspective(glm::radians(60.0f), aspect, 0.1f, 600.0f);

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
    Engine::ImGuiLayer *layer = GetImGuiLayer();
    if (!layer || !layer->isInitialized() || ImGui::GetCurrentContext() == nullptr)
        return;

    // ---- Group HUD: per-local-group health bars above units ----
    if (!m_inGame)
        return;

    ImGuiIO &io = ImGui::GetIO();

    // Transparent overlay window covering full screen
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##GroupHUD", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    ImDrawList *draw = ImGui::GetWindowDrawList();

    auto &ecs = GetECS();
    const uint32_t disabledId = ecs.components.ensureId("Disabled");
    const uint32_t deadId = ecs.components.ensureId("Dead");

    // Grouping: cluster by (team + proximity) using spatial index.
    // This matches the selection logic and supports multiple groups per team.
    const float linkR2 = SampleTuning::HUD_GROUP_LINK_RADIUS_M * SampleTuning::HUD_GROUP_LINK_RADIUS_M;

    const float maxHPPerUnit = std::max(1.0f, m_systems.GetCombatSystem().config().maxHPPerUnit);

    auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
    {
        return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
    };

    struct GroupStats
    {
        uint8_t team = 0;
        int alive = 0;
        float totalHP = 0.0f;
        glm::vec3 centroid{0.0f};
    };

    std::vector<GroupStats> groups;
    groups.reserve(64);

    std::unordered_set<uint64_t> visited;
    visited.reserve(2048);

    const auto &spatial = m_systems.GetSpatialIndex();

    // Enumerate all living units that have Team+Health+Position.
    for (const auto &ptr : ecs.stores.stores())
    {
        if (!ptr)
            continue;

        const auto &store = *ptr;
        if (!store.hasPosition() || !store.hasHealth() || !store.hasTeam())
            continue;
        if (store.signature().has(disabledId) || store.signature().has(deadId))
            continue;

        const auto &ents = store.entities();
        const auto &pos = store.positions();
        const auto &hp = store.healths();
        const auto &teams = store.teams();
        const uint32_t n = store.size();

        for (uint32_t row = 0; row < n; ++row)
        {
            if (hp[row].value <= 0.0f)
                continue;

            const Engine::ECS::Entity seed = ents[row];
            const uint64_t seedKey = entityKey(seed);
            if (visited.find(seedKey) != visited.end())
                continue;

            const uint8_t teamId = teams[row].id;

            // Flood-fill cluster.
            std::vector<Engine::ECS::Entity> queue;
            queue.reserve(128);
            queue.push_back(seed);
            visited.insert(seedKey);

            int aliveCount = 0;
            float hpSum = 0.0f;
            glm::vec3 sumPos{0.0f};

            while (!queue.empty())
            {
                const Engine::ECS::Entity cur = queue.back();
                queue.pop_back();

                auto *rec = ecs.entities.find(cur);
                if (!rec)
                    continue;
                auto *st = ecs.stores.get(rec->archetypeId);
                if (!st || rec->row >= st->size())
                    continue;
                if (!st->hasPosition() || !st->hasHealth() || !st->hasTeam())
                    continue;
                if (st->signature().has(disabledId) || st->signature().has(deadId))
                    continue;
                if (st->healths()[rec->row].value <= 0.0f)
                    continue;
                if (st->teams()[rec->row].id != teamId)
                    continue;

                const float cx = st->positions()[rec->row].x;
                const float cy = st->positions()[rec->row].y;
                const float cz = st->positions()[rec->row].z;

                aliveCount += 1;
                hpSum += std::max(0.0f, st->healths()[rec->row].value);
                sumPos += glm::vec3(cx, cy, cz);

                spatial.forNeighborsInRadius(cx, cz, SampleTuning::HUD_GROUP_LINK_RADIUS_M, [&](uint32_t nStoreId, uint32_t nRow)
                                             {
                                                 auto *ns = ecs.stores.get(nStoreId);
                                                 if (!ns)
                                                     return;
                                                 if (nRow >= ns->size())
                                                     return;
                                                 if (!ns->hasPosition() || !ns->hasHealth() || !ns->hasTeam())
                                                     return;
                                                 if (ns->signature().has(disabledId) || ns->signature().has(deadId))
                                                     return;
                                                 if (ns->healths()[nRow].value <= 0.0f)
                                                     return;
                                                 if (ns->teams()[nRow].id != teamId)
                                                     return;

                                                 const float dx = ns->positions()[nRow].x - cx;
                                                 const float dz = ns->positions()[nRow].z - cz;
                                                 if (dx * dx + dz * dz > linkR2)
                                                     return;

                                                 const Engine::ECS::Entity cand = ns->entities()[nRow];
                                                 const uint64_t k = entityKey(cand);
                                                 if (visited.insert(k).second)
                                                     queue.push_back(cand); });
            }

            if (aliveCount <= 0)
                continue;

            GroupStats gs;
            gs.team = teamId;
            gs.alive = aliveCount;
            gs.totalHP = hpSum;
            gs.centroid = sumPos * (1.0f / static_cast<float>(aliveCount));
            groups.push_back(gs);
        }
    }

    // Draw a thin translucent bar for each group.
    const glm::mat4 view = m_camera.GetViewMatrix();
    const glm::mat4 proj = m_camera.GetProjectionMatrix();
    const glm::mat4 vp = proj * view;

    const float screenW = io.DisplaySize.x;
    const float screenH = io.DisplaySize.y;

    const float kBarW = SampleTuning::HUD_BAR_WIDTH_PX;
    const float kBarH = SampleTuning::HUD_BAR_HEIGHT_PX;

    for (const auto &g : groups)
    {
        const glm::vec4 world(g.centroid.x, g.centroid.y + SampleTuning::HUD_WORLD_HEIGHT_OFFSET_M, g.centroid.z, 1.0f);
        const glm::vec4 clip = vp * world;
        if (clip.w <= 1e-6f)
            continue;

        const glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (ndc.x < -1.1f || ndc.x > 1.1f || ndc.y < -1.1f || ndc.y > 1.1f)
            continue;

        const float sx = (ndc.x * 0.5f + 0.5f) * screenW;
        const float sy = (ndc.y * 0.5f + 0.5f) * screenH;

        const float maxHP = std::max(1.0f, static_cast<float>(g.alive) * maxHPPerUnit);
        const float frac = std::clamp(g.totalHP / maxHP, 0.0f, 1.0f);

        const ImU32 fill = (g.team == 0) ? SampleTuning::HUD_TEAM0_FILL : SampleTuning::HUD_TEAM1_FILL;
        const ImU32 bg = (g.team == 0) ? SampleTuning::HUD_TEAM0_BG : SampleTuning::HUD_TEAM1_BG;
        const ImU32 border = SampleTuning::HUD_BORDER;

        const float x0 = sx - kBarW * 0.5f;
        const float x1 = sx + kBarW * 0.5f;
        const float y0 = sy;
        const float y1 = sy + kBarH;

        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), bg, SampleTuning::HUD_BAR_CORNER_ROUNDING_PX);
        draw->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + kBarW * frac, y1), fill, SampleTuning::HUD_BAR_CORNER_ROUNDING_PX);
        draw->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), border, SampleTuning::HUD_BAR_CORNER_ROUNDING_PX, 0, SampleTuning::HUD_BAR_BORDER_THICKNESS_PX);

        char buf[96];
        snprintf(buf, sizeof(buf), "%d  %.0f", g.alive, std::max(0.0f, g.totalHP));
        const ImVec2 ts = ImGui::CalcTextSize(buf);
        draw->AddText(ImVec2(sx - ts.x * 0.5f, y1 + SampleTuning::HUD_TEXT_OFFSET_Y_PX), SampleTuning::HUD_TEXT, buf);
    }

    ImGui::End();
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
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cerr << "[Prefab] Failed to read: " << path << "\n";
#endif
                continue;
            }
            Engine::ECS::Prefab p = Engine::ECS::loadPrefabFromJson(jsonText, ecs.components, ecs.archetypes, *m_assets);
            if (p.name.empty())
            {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cerr << "[Prefab] Missing name in: " << path << "\n";
#endif
                continue;
            }
            ecs.prefabs.add(p);
            ++prefabCount;
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            std::cout << "[Prefab] Loaded " << p.name << " from " << path << "\n";
#endif
        }
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Prefab] Failed to enumerate entities/: " << e.what() << "\n";
#endif
        return;
    }

    if (prefabCount == 0)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Prefab] No prefabs loaded from entities/*.json\n";
#endif
        return;
    }

    Sample::SpawnFromScenarioFile(ecs, "BattleConfig.json", /*selectSpawned=*/false);

    // --- Load combat tuning from BattleConfig.json ---
    try
    {
        std::ifstream cfgFile("BattleConfig.json");
        if (cfgFile.is_open())
        {
            nlohmann::json root = nlohmann::json::parse(cfgFile);
            if (root.contains("combat") && root["combat"].is_object())
            {
                const auto &c = root["combat"];
                CombatSystem::CombatConfig cfg;
                if (c.contains("meleeRange"))
                    cfg.meleeRange = c["meleeRange"].get<float>();
                if (c.contains("engageRange"))
                    cfg.engageRange = c["engageRange"].get<float>();
                if (c.contains("damageMin"))
                    cfg.damageMin = c["damageMin"].get<float>();
                if (c.contains("damageMax"))
                    cfg.damageMax = c["damageMax"].get<float>();
                // Legacy single-value fallback
                if (c.contains("damagePerHit") && !c.contains("damageMin"))
                {
                    float d = c["damagePerHit"].get<float>();
                    cfg.damageMin = d * 0.6f;
                    cfg.damageMax = d * 1.4f;
                }
                if (c.contains("deathRemoveDelay"))
                    cfg.deathRemoveDelay = c["deathRemoveDelay"].get<float>();
                if (c.contains("maxHPPerUnit"))
                    cfg.maxHPPerUnit = c["maxHPPerUnit"].get<float>();
                if (c.contains("missChance"))
                    cfg.missChance = c["missChance"].get<float>();
                if (c.contains("critChance"))
                    cfg.critChance = c["critChance"].get<float>();
                if (c.contains("critMultiplier"))
                    cfg.critMultiplier = c["critMultiplier"].get<float>();
                if (c.contains("rageMaxBonus"))
                    cfg.rageMaxBonus = c["rageMaxBonus"].get<float>();
                if (c.contains("cooldownJitter"))
                    cfg.cooldownJitter = c["cooldownJitter"].get<float>();
                if (c.contains("staggerMax"))
                    cfg.staggerMax = c["staggerMax"].get<float>();
                m_systems.GetCombatSystemMut().applyConfig(cfg);
                m_systems.GetCombatSystemMut().setHumanTeam(0); // Team A = human player
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "[Config] Combat config loaded from BattleConfig.json\n";
#endif
            }

            // Load start zone (click here to begin battle)
            if (root.contains("startZone") && root["startZone"].is_object())
            {
                const auto &sz = root["startZone"];
                m_startZoneX = sz.value("x", 0.0f);
                m_startZoneZ = sz.value("z", 0.0f);
                m_startZoneRadius = sz.value("radius", 10.0f);
                m_hasStartZone = true;
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
                std::cout << "[Config] Start zone at (" << m_startZoneX << "," << m_startZoneZ
                          << ") r=" << m_startZoneRadius << "\n";
#endif
            }
        }
    }
    catch (const std::exception &e)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cerr << "[Config] Failed to parse combat config: " << e.what() << "\n";
#endif
    }
}

void MySampleApp::OnEvent(const std::string &name)
{
    std::istringstream iss(name);
    std::string evt;
    iss >> evt;

    if (evt == "F1Pressed")
    {
        TogglePerformanceMonitorOverlay();
        return;
    }

    if (evt == "MouseButtonLeftDown")
    {
        // Skip game input when ImGui is using the mouse (editor sliders, buttons, etc.)
        if (ImGui::GetIO().WantCaptureMouse)
            return;

        // Left click is reserved for camera drag/pan only.
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
        if (ImGui::GetIO().WantCaptureMouse)
            return;

        PickAndSelectEntityAtCursor();
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

    if (name == "WindowResize")
    {
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
    GLFWwindow *wnd = static_cast<GLFWwindow *>(GetWindow().GetWindowPointer());
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
    if (!i.good())
        return;
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
    GLFWwindow *wnd = static_cast<GLFWwindow *>(GetWindow().GetWindowPointer());
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