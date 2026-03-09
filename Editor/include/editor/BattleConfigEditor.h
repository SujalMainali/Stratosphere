#pragma once

#include "ECS/ECSContext.h"
#include "editor/GameWorldSpawner.h"

#include <filesystem>
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
        void drawSpawnGroupsSection();
        void drawControlButtons(Engine::ECS::ECSContext &ecs);

        void writeWorkingCopy(const std::string &outPath);
        bool writePermanent();
        void reloadFromDisk(Engine::ECS::ECSContext &ecs);

        void respawnFromPath(Engine::ECS::ECSContext &ecs, const std::string &scenarioPath);
        void respawnWorkingCopy(Engine::ECS::ECSContext &ecs);
        void resetGame(Engine::ECS::ECSContext &ecs);
        void clearAllEntities(Engine::ECS::ECSContext &ecs);

        struct CombatConfig
        {
            float meleeRange = 2.0f;
            float engageRange = 10.0f;
            float damageMin = 12.0f;
            float damageMax = 28.0f;
            float deathRemoveDelay = 3.0f;
            float maxHPPerUnit = 140.0f;
            float missChance = 0.20f;
            float critChance = 0.10f;
            float critMultiplier = 2.0f;
            float rageMaxBonus = 0.50f;
            float cooldownJitter = 0.30f;
            float staggerMax = 0.60f;
        };

        CombatConfig m_combat;
        CombatConfig m_originalCombat;

        struct SpawnGroupParams
        {
            int count = 20;
            float offsetX = 0.0f;
            float offsetZ = 0.0f;
            int columns = 5;
            float spacing = 5.0f;
            float jitter = 0.2f;
        };

        SpawnGroupParams m_teamA;
        SpawnGroupParams m_originalTeamA;
        SpawnGroupParams m_teamB;
        SpawnGroupParams m_originalTeamB;

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
