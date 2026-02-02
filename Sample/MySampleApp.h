#pragma once

#include "Engine/Application.h"
#include "Engine/Camera.h"
#include "src/MenuManager.h"

#include "update.h"

#include "assets/Handles.h"

#include <glm/glm.hpp>

#include <memory>
#include <vector>

namespace Engine
{
    class AssetManager;
    class GroundPlaneRenderPassModule;
}

class MySampleApp : public Engine::Application
{
public:
    MySampleApp();
    ~MySampleApp() override;

    void Close() override;
    void OnUpdate(Engine::TimeStep ts) override;
    void OnRender() override;

private:
    void setupECSFromPrefabs();
    void OnEvent(const std::string &name);
    void ApplyRTSCamera(float aspect);
    void PickAndSelectEntityAtCursor();

private:
    struct RTSCameraController
    {
        // Ground-plane focus point (y is always 0).
        glm::vec3 focus{0.0f, 0.0f, 0.0f};

        // Orientation (kept stable; panning/zoom do not change this).
        float yawDeg = -45.0f;
        float pitchDeg = -55.0f;

        // Zoom model: camera height above ground.
        float height = 70.0f;

        // Tuning
        float basePanSpeed = 0.0020f;
        float zoomSpeed = 5.0f;
        float minHeight = 5.0f;
        float maxHeight = 250.0f;
    };

    std::unique_ptr<Engine::AssetManager> m_assets;
    RTSCameraController m_rtsCam;
    glm::vec2 m_lastMouse{0.0f, 0.0f};
    bool m_isPanning = false;
    bool m_panJustStarted = false;
    float m_scrollDelta = 0.0f;
    Engine::Camera m_camera;

    // Simple background ground plane
    Engine::TextureHandle m_groundTexture;
    std::shared_ptr<Engine::GroundPlaneRenderPassModule> m_groundPass;

    Sample::SystemRunner m_systems;

        // Menu
    MenuManager m_menu;

    // Small save slot filename
    std::string m_saveFilePath = "sample_save.json";

    // Helpers
    void SaveGameState();
    void LoadGameState();
    bool HasSaveFile() const;
};