#pragma once

#include "Engine/Application.h"
#include "Engine/Camera.h"

#include "update.h"
#include "editor/BattleConfigEditor.h"

#include "assets/Handles.h"

#include <glm/glm.hpp>

#include <memory>

namespace Engine
{
    class AssetManager;
    class GroundPlaneRenderPassModule;
}

class EditorApp : public Engine::Application
{
public:
    explicit EditorApp(std::string battleConfigPath);
    ~EditorApp() override;

    void Close() override;
    void OnUpdate(Engine::TimeStep ts) override;
    void OnRender() override;

private:
    void setupECSFromPrefabs();
    void applyCombatConfigFromFile();
    void OnEvent(const std::string &name);
    void ApplyRTSCamera(float aspect);

private:
    struct RTSCameraController
    {
        glm::vec3 focus{0.0f, 0.0f, 0.0f};
        float yawDeg = -45.0f;
        float pitchDeg = -55.0f;
        float height = 120.0f;

        float basePanSpeed = 0.0020f;
        float zoomSpeed = 5.0f;
        float minHeight = 5.0f;
        float maxHeight = 250.0f;
    };

    std::string m_battleConfigPath;

    std::unique_ptr<Engine::AssetManager> m_assets;
    RTSCameraController m_rtsCam;
    glm::vec2 m_lastMouse{0.0f, 0.0f};
    bool m_isPanning = false;
    bool m_panJustStarted = false;
    float m_scrollDelta = 0.0f;
    Engine::Camera m_camera;

    Engine::TextureHandle m_groundTexture;
    std::shared_ptr<Engine::GroundPlaneRenderPassModule> m_groundPass;

    Sample::SystemRunner m_systems;
    Editor::BattleConfigEditor m_configEditor;
};
