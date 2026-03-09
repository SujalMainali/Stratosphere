#pragma once

#include "systems/CombatSystem.h"
#include "ECS/ECSContext.h"
#include "ScenarioSpawner.h"

#include <string>
#include <filesystem>
#include <chrono>

namespace Sample
{
    class SystemRunner;

    // ImGui-based runtime editor for BattleConfig.json combat tuning
    // and CombatKnight.json unit parameters.  F1 toggles visibility.
    class BattleConfigEditor
    {
    public:
        BattleConfigEditor();

        // Load combat tuning from BattleConfig.json.
        void loadFromFile(const std::string &path = "BattleConfig.json");

        // Load unit defaults from a prefab JSON (e.g. CombatKnight.json).
        void loadUnitConfig(const std::string &path = "entities/CombatKnight.json");

        // Draw the editor panel (no-op when hidden).  Call inside OnRender().
        void draw(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        void toggleVisible() { m_visible = !m_visible; }
        bool isVisible() const { return m_visible; }

    private:
        void drawCombatSection();
        void drawSpawnGroupsSection();
        void drawControlButtons(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        void writeLiveConfig();
        void reloadFromDisk(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        void applyToSystems(SystemRunner &systems);
        void applyUnitParamsToEntities(Engine::ECS::ECSContext &ecs);
        void respawn(Engine::ECS::ECSContext &ecs, SystemRunner &systems);
        void resetGame(Engine::ECS::ECSContext &ecs, SystemRunner &systems);
        void clearAllEntities(Engine::ECS::ECSContext &ecs);

        // --- Combat tuning (mirrors CombatConfig) ---
        CombatSystem::CombatConfig m_combat;
        CombatSystem::CombatConfig m_originalCombat;

        // --- Per-team spawn group parameters (from BattleConfig.json) ---
        struct UnitParams
        {
            float health         = 140.0f;
            float moveSpeed      = 5.0f;
            float radius         = 1.5f;
            float separation     = 1.0f;
            float attackInterval = 1.5f;
        };

        struct SpawnGroupParams
        {
            int   count      = 20;
            float offsetX    = 0.0f;
            float offsetZ    = 0.0f;
            int   columns    = 5;
            float spacing    = 5.0f;
            float jitter     = 0.2f;
            UnitParams unit;
        };
        SpawnGroupParams m_teamA;
        SpawnGroupParams m_originalTeamA;
        SpawnGroupParams m_teamB;
        SpawnGroupParams m_originalTeamB;

        // --- State ---
        bool m_visible = true;
        std::string m_battleConfigPath = "BattleConfig.json";
        std::string m_liveConfigPath;

        std::string m_statusMsg;
        float m_statusTimer = 0.0f;

        // File-watcher state (polling-based)
        std::filesystem::file_time_type m_lastWriteTime{};
        float m_watchPollTimer = 0.0f;
        static constexpr float kWatchPollInterval = 0.5f; // seconds
    };
}
