#pragma once

#include "systems/CombatSystem.h"
#include "ECS/ECSContext.h"
#include "ScenarioSpawner.h"

#include <string>

namespace Sample
{
    class SystemRunner;

    // ImGui-based runtime editor for BattleConfig.json parameters.
    // Skips obstacles. Allows editing combat tuning, anchors, and spawn groups,
    // with Apply, Reset-to-file, and Restart-battle buttons.
    class BattleConfigEditor
    {
    public:
        BattleConfigEditor();

        // Load initial values from BattleConfig.json on disk.
        void loadFromFile(const std::string &path = "BattleConfig.json");

        // Draw the editor panel.  Call this inside OnRender().
        void draw(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        // Read-only access to the live combat config held by the editor.
        const CombatSystem::CombatConfig &combatConfig() const { return m_combat; }

    private:
        void drawCombatSection();
        void drawAnchorsSection();
        void drawSpawnGroupsSection();
        void drawControlButtons(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        // Applies the current editor values to the running game systems.
        void applyToSystems(SystemRunner &systems);

        // Destroys all entities and re-spawns from the original config file, restarting the battle.
        void restartBattle(Engine::ECS::ECSContext &ecs, SystemRunner &systems);

        // Helper: removes every entity from all archetype stores.
        void clearAllEntities(Engine::ECS::ECSContext &ecs);

        // --- Combat tuning (mirrors CombatConfig) ---
        CombatSystem::CombatConfig m_combat;

        // --- Anchors ---
        struct Anchor
        {
            std::string name;
            float x = 0.0f;
            float z = 0.0f;
        };
        std::vector<Anchor> m_anchors;

        // --- Spawn Groups ---
        struct SpawnGroupUI
        {
            std::string id;
            std::string unitType;
            int count = 0;
            std::string anchorName;
            float offsetX = 0.0f;
            float offsetZ = 0.0f;
            float facingYawDeg = 0.0f;
            int team = 0;

            // Formation
            std::string formationKind = "grid";
            int columns = 5;
            float spacingM = 5.0f;
            bool spacingAuto = false;
            float jitterM = 0.2f;
            float circleRadiusM = 0.0f;
        };
        std::vector<SpawnGroupUI> m_spawnGroups;

        // Path used for load/save.
        std::string m_filePath = "BattleConfig.json";

        // Status message shown briefly after an action.
        std::string m_statusMsg;
        float m_statusTimer = 0.0f;
    };
}
