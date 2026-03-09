#pragma once

#include "ECS/ECSContext.h"
#include "editor/GameWorldSpawner.h"

#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>

namespace Editor
{
    // ImGui-based editor for GameWorld.json combat tuning and spawn groups.
    // Intended to run in the standalone EditorApp.
    class BattleConfigEditor
    {
    public:
        BattleConfigEditor();

        // Load combat tuning and spawn groups from GameWorld.json.
        void loadFromFile(const std::string &path = "GameWorld.json");

        // Draw the editor panel (no-op when hidden). Call inside Application::OnRender().
        void draw(Engine::ECS::ECSContext &ecs);

        void toggleVisible() { m_visible = !m_visible; }
        bool isVisible() const { return m_visible; }

    private:
        void drawCombatSection();
        void drawAnchorsSection();
        void drawSpawnGroupsSection();
        void drawObstaclesSection();
        void drawControlButtons(Engine::ECS::ECSContext &ecs);

        void writeWorkingCopy(const std::string &outPath);
        bool writePermanent();
        void reloadFromDisk(Engine::ECS::ECSContext &ecs);

        void respawnFromPath(Engine::ECS::ECSContext &ecs, const std::string &scenarioPath);
        void respawnWorkingCopy(Engine::ECS::ECSContext &ecs);
        void resetGame(Engine::ECS::ECSContext &ecs);
        void clearAllEntities(Engine::ECS::ECSContext &ecs);

        // Working copy of the loaded file (source-of-truth for all edits).
        nlohmann::json m_doc;
        nlohmann::json m_originalDoc;

        // Selection state for list-style editors.
        std::string m_selectedAnchorKey;
        int m_selectedSpawnGroupIndex = -1;
        int m_selectedObstacleIndex = -1;
        int m_selectedGapIndex = -1;

        // UI buffers
        char m_newAnchorName[64] = {0};
        char m_renameAnchorName[64] = {0};
        char m_newSpawnGroupId[64] = {0};
        char m_newObstaclePrefab[64] = {0};

        bool m_visible = true;
        std::string m_battleConfigPath = "GameWorld.json";
        std::string m_workingCopyPath;

        std::string m_statusMsg;
        float m_statusTimer = 0.0f;

        std::filesystem::file_time_type m_battleLastWriteTime{};
        float m_watchPollTimer = 0.0f;
        static constexpr float kWatchPollInterval = 0.5f;
    };
}
