#include "editor/BattleConfigEditor.h"

#include "update.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>

using json = nlohmann::json;

namespace Editor
{

    BattleConfigEditor::BattleConfigEditor() = default;

    void BattleConfigEditor::loadFromFile(const std::string &path)
    {
        m_battleConfigPath = path;

        // Derive a working-copy path (e.g. BattleConfig.json -> BattleConfig_working.json)
        {
            std::filesystem::path p(path);
            auto stem = p.stem().string();
            auto ext = p.extension().string();
            m_workingCopyPath = (p.parent_path() / (stem + "_working" + ext)).string();
        }

        std::ifstream file(path);
        if (!file.is_open())
        {
            m_statusMsg = "Failed to open " + path;
            m_statusTimer = 3.0f;
            return;
        }

        json root;
        try
        {
            root = json::parse(file);
        }
        catch (const std::exception &e)
        {
            m_statusMsg = std::string("JSON parse error: ") + e.what();
            m_statusTimer = 4.0f;
            return;
        }

        if (root.contains("combat") && root["combat"].is_object())
        {
            const auto &c = root["combat"];
            auto g = [&](const char *k, float def)
            { return c.contains(k) ? c[k].get<float>() : def; };
            m_combat.meleeRange = g("meleeRange", CombatTuning::MELEE_RANGE_DEFAULT);
            m_combat.engageRange = g("engageRange", CombatTuning::ENGAGE_RANGE_DEFAULT);
            m_combat.damageMin = g("damageMin", CombatTuning::DAMAGE_MIN_DEFAULT);
            m_combat.damageMax = g("damageMax", CombatTuning::DAMAGE_MAX_DEFAULT);
            m_combat.deathRemoveDelay = g("deathRemoveDelay", CombatTuning::DEATH_REMOVE_DELAY_DEFAULT);
            m_combat.maxHPPerUnit = g("maxHPPerUnit", CombatTuning::MAX_HP_PER_UNIT_DEFAULT);
            m_combat.missChance = g("missChance", CombatTuning::MISS_CHANCE_DEFAULT);
            m_combat.critChance = g("critChance", CombatTuning::CRIT_CHANCE_DEFAULT);
            m_combat.critMultiplier = g("critMultiplier", CombatTuning::CRIT_MULTIPLIER_DEFAULT);
            m_combat.rageMaxBonus = g("rageMaxBonus", CombatTuning::RAGE_MAX_BONUS_DEFAULT);
            m_combat.cooldownJitter = g("cooldownJitter", CombatTuning::COOLDOWN_JITTER_DEFAULT);
            m_combat.staggerMax = g("staggerMax", CombatTuning::STAGGER_MAX_DEFAULT);

            if (c.contains("damagePerHit") && !c.contains("damageMin"))
            {
                float d = c["damagePerHit"].get<float>();
                m_combat.damageMin = d * 0.6f;
                m_combat.damageMax = d * 1.4f;
            }
        }

        m_originalCombat = m_combat;

        if (root.contains("spawnGroups") && root["spawnGroups"].is_array())
        {
            for (const auto &sg : root["spawnGroups"])
            {
                std::string id = sg.value("id", std::string(""));
                SpawnGroupParams params;
                params.count = sg.value("count", 20);
                params.offsetX = sg.contains("offset") ? sg["offset"].value("x", 0.0f) : 0.0f;
                params.offsetZ = sg.contains("offset") ? sg["offset"].value("z", 0.0f) : 0.0f;
                if (sg.contains("formation") && sg["formation"].is_object())
                {
                    const auto &f = sg["formation"];
                    params.columns = f.value("columns", 5);
                    params.spacing = f.contains("spacing_m") && f["spacing_m"].is_number() ? f["spacing_m"].get<float>() : 5.0f;
                    params.jitter = f.value("jitter_m", 0.2f);
                }

                if (id == "team_a")
                    m_teamA = params;
                else if (id == "team_b")
                    m_teamB = params;
            }
        }

        m_originalTeamA = m_teamA;
        m_originalTeamB = m_teamB;

        m_statusMsg = "Loaded " + path;
        m_statusTimer = 2.0f;

        try
        {
            m_battleLastWriteTime = std::filesystem::last_write_time(m_battleConfigPath);
        }
        catch (...)
        {
        }
    }

    void BattleConfigEditor::loadUnitConfig(const std::string &path)
    {
        m_unitConfigPath = path;

        std::ifstream file(path);
        if (!file.is_open())
            return;

        json root;
        try
        {
            root = json::parse(file);
        }
        catch (...)
        {
            return;
        }

        if (root.contains("defaults") && root["defaults"].is_object())
        {
            const auto &d = root["defaults"];
            UnitParams u;
            if (d.contains("Health"))
                u.health = d["Health"].value("value", 140.0f);
            if (d.contains("MoveSpeed"))
                u.moveSpeed = d["MoveSpeed"].value("value", 5.0f);
            if (d.contains("Radius"))
                u.radius = d["Radius"].value("r", 1.5f);
            if (d.contains("Separation"))
                u.separation = d["Separation"].value("value", 1.0f);
            if (d.contains("AttackCooldown"))
                u.attackInterval = d["AttackCooldown"].value("interval", 1.5f);

            m_teamA.unit = u;
            m_teamB.unit = u;
        }

        m_originalTeamA = m_teamA;
        m_originalTeamB = m_teamB;

        try
        {
            m_unitLastWriteTime = std::filesystem::last_write_time(m_unitConfigPath);
        }
        catch (...)
        {
        }
    }

    void BattleConfigEditor::draw(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems)
    {
        // --- File-watcher: poll for external edits to config files ---
        {
            float dt = ImGui::GetIO().DeltaTime;
            m_watchPollTimer += dt;
            if (m_watchPollTimer >= kWatchPollInterval)
            {
                m_watchPollTimer = 0.0f;
                bool changed = false;
                try
                {
                    auto t = std::filesystem::last_write_time(m_battleConfigPath);
                    if (t != m_battleLastWriteTime)
                    {
                        m_battleLastWriteTime = t;
                        changed = true;
                    }
                }
                catch (...)
                {
                }
                try
                {
                    auto t = std::filesystem::last_write_time(m_unitConfigPath);
                    if (t != m_unitLastWriteTime)
                    {
                        m_unitLastWriteTime = t;
                        changed = true;
                    }
                }
                catch (...)
                {
                }

                if (changed)
                    reloadFromDisk(ecs, systems);
            }
        }

        if (!m_visible)
            return;

        if (m_statusTimer > 0.0f)
        {
            float dt = ImGui::GetIO().DeltaTime;
            m_statusTimer -= dt;
            if (m_statusTimer < 0.0f)
                m_statusTimer = 0.0f;
        }

        ImGuiIO &io = ImGui::GetIO();

        const float panelWidth = 420.0f;
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelWidth, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y));

        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("Editor: BattleConfig", nullptr, flags);
        ImGui::PopStyleColor();

        drawControlButtons(ecs, systems);
        ImGui::Separator();
        drawCombatSection();
        ImGui::Separator();
        drawSpawnGroupsSection();

        if (m_statusTimer > 0.0f && !m_statusMsg.empty())
        {
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", m_statusMsg.c_str());
        }

        ImGui::End();
    }

    void BattleConfigEditor::drawCombatSection()
    {
        if (!ImGui::CollapsingHeader("Combat Tuning", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::PushItemWidth(-140);

        ImGui::Text("Ranges");
        ImGui::SliderFloat("Melee Range", &m_combat.meleeRange, 0.5f, 30.0f, "%.1f");
        ImGui::SliderFloat("Engage Range", &m_combat.engageRange, 1.0f, 60.0f, "%.1f");

        ImGui::Spacing();
        ImGui::Text("Damage");
        ImGui::SliderFloat("Damage Min", &m_combat.damageMin, 1.0f, 100.0f, "%.1f");
        ImGui::SliderFloat("Damage Max", &m_combat.damageMax, 1.0f, 200.0f, "%.1f");
        ImGui::SliderFloat("Max HP / Unit", &m_combat.maxHPPerUnit, 10.0f, 1000.0f, "%.0f");

        ImGui::Spacing();
        ImGui::Text("Chances");
        ImGui::SliderFloat("Miss Chance", &m_combat.missChance, 0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Crit Chance", &m_combat.critChance, 0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderFloat("Crit Multiplier", &m_combat.critMultiplier, 1.0f, 5.0f, "%.2f");

        ImGui::Spacing();
        ImGui::Text("Timing & Modifiers");
        ImGui::SliderFloat("Death Remove Delay", &m_combat.deathRemoveDelay, 0.0f, 15.0f, "%.1f s");
        ImGui::SliderFloat("Rage Max Bonus", &m_combat.rageMaxBonus, 0.0f, 1.0f, "%.0f%%");
        ImGui::SliderFloat("Cooldown Jitter", &m_combat.cooldownJitter, 0.0f, 1.0f, "%.0f%%");
        ImGui::SliderFloat("Stagger Max", &m_combat.staggerMax, 0.0f, 2.0f, "%.2f s");

        ImGui::PopItemWidth();
    }

    void BattleConfigEditor::drawSpawnGroupsSection()
    {
        if (!ImGui::CollapsingHeader("Spawn Groups", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ImGui::PushItemWidth(-140);

        auto drawTeam = [](const char *label, SpawnGroupParams &p)
        {
            ImGui::PushID(label);
            if (ImGui::CollapsingHeader(label))
            {
                ImGui::SliderInt("Count", &p.count, 1, 1000);
                ImGui::SliderFloat("Offset X", &p.offsetX, -50.0f, 50.0f, "%.1f");
                ImGui::SliderFloat("Offset Z", &p.offsetZ, -50.0f, 50.0f, "%.1f");
                ImGui::SliderInt("Columns", &p.columns, 1, 20);
                ImGui::SliderFloat("Spacing", &p.spacing, 1.0f, 20.0f, "%.1f m");
                ImGui::SliderFloat("Jitter", &p.jitter, 0.0f, 3.0f, "%.2f m");
                ImGui::Spacing();
                ImGui::Text("Unit Parameters");
                ImGui::SliderFloat("Health", &p.unit.health, 1.0f, 1000.0f, "%.0f");
                ImGui::SliderFloat("Move Speed", &p.unit.moveSpeed, 0.5f, 30.0f, "%.1f");
                ImGui::SliderFloat("Radius", &p.unit.radius, 0.1f, 10.0f, "%.2f");
                ImGui::SliderFloat("Separation", &p.unit.separation, 0.0f, 10.0f, "%.2f");
                ImGui::SliderFloat("Attack Interval", &p.unit.attackInterval, 0.1f, 10.0f, "%.2f s");
            }
            ImGui::PopID();
        };

        drawTeam("Team A", m_teamA);
        drawTeam("Team B", m_teamB);

        ImGui::PopItemWidth();
    }

    void BattleConfigEditor::writeWorkingCopy(const std::string &outPath)
    {
        std::ifstream inFile(m_battleConfigPath);
        if (!inFile.is_open())
            return;

        json root;
        try
        {
            root = json::parse(inFile);
        }
        catch (...)
        {
            return;
        }

        inFile.close();

        root["combat"]["meleeRange"] = m_combat.meleeRange;
        root["combat"]["engageRange"] = m_combat.engageRange;
        root["combat"]["damageMin"] = m_combat.damageMin;
        root["combat"]["damageMax"] = m_combat.damageMax;
        root["combat"]["deathRemoveDelay"] = m_combat.deathRemoveDelay;
        root["combat"]["maxHPPerUnit"] = m_combat.maxHPPerUnit;
        root["combat"]["missChance"] = m_combat.missChance;
        root["combat"]["critChance"] = m_combat.critChance;
        root["combat"]["critMultiplier"] = m_combat.critMultiplier;
        root["combat"]["rageMaxBonus"] = m_combat.rageMaxBonus;
        root["combat"]["cooldownJitter"] = m_combat.cooldownJitter;
        root["combat"]["staggerMax"] = m_combat.staggerMax;

        if (root.contains("spawnGroups") && root["spawnGroups"].is_array())
        {
            for (auto &sg : root["spawnGroups"])
            {
                std::string id = sg.value("id", std::string(""));
                const SpawnGroupParams *p = nullptr;
                if (id == "team_a")
                    p = &m_teamA;
                else if (id == "team_b")
                    p = &m_teamB;
                else
                    continue;

                sg["count"] = p->count;
                sg["offset"]["x"] = p->offsetX;
                sg["offset"]["z"] = p->offsetZ;
                if (!sg.contains("formation"))
                    sg["formation"] = json::object();
                sg["formation"]["columns"] = p->columns;
                sg["formation"]["spacing_m"] = p->spacing;
                sg["formation"]["jitter_m"] = p->jitter;
            }
        }

        std::ofstream outFile(outPath);
        if (outFile.is_open())
            outFile << root.dump(4) << std::endl;
    }

    bool BattleConfigEditor::writePermanent()
    {
        // Best-effort: write directly to BattleConfig.json path.
        // If the file is read-only or locked, this will fail.
        std::ofstream test(m_battleConfigPath, std::ios::app);
        if (!test.good())
            return false;
        test.close();

        writeWorkingCopy(m_battleConfigPath);

        try
        {
            m_battleLastWriteTime = std::filesystem::last_write_time(m_battleConfigPath);
        }
        catch (...)
        {
        }

        // Update "original" snapshots to match what we just saved.
        m_originalCombat = m_combat;
        m_originalTeamA = m_teamA;
        m_originalTeamB = m_teamB;

        return true;
    }

    void BattleConfigEditor::drawControlButtons(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems)
    {
        if (ImGui::Button("Apply (Preview)"))
        {
            respawnWorkingCopy(ecs, systems);
            m_statusMsg = "Preview applied (respawned from working copy)";
            m_statusTimer = 3.0f;
        }

        ImGui::SameLine();

        if (ImGui::Button("Save to BattleConfig.json"))
        {
            if (writePermanent())
            {
                respawnFromPath(ecs, systems, m_battleConfigPath);
                m_statusMsg = "Saved to BattleConfig.json";
            }
            else
            {
                m_statusMsg = "Save failed (check permissions)";
            }
            m_statusTimer = 3.0f;
        }

        ImGui::SameLine();

        if (ImGui::Button("Reset"))
        {
            resetGame(ecs, systems);
            m_statusMsg = "Reset to last loaded/saved config";
            m_statusTimer = 3.0f;
        }
    }

    void BattleConfigEditor::applyToSystems(Sample::SystemRunner &systems)
    {
        m_combat.maxHPPerUnit = std::max(m_teamA.unit.health, m_teamB.unit.health);
        systems.GetCombatSystemMut().applyConfig(m_combat);

        systems.GetCombatSystemMut().setTeamTotalSpawned(0, m_teamA.count);
        systems.GetCombatSystemMut().setTeamMaxHP(0, m_teamA.count * m_teamA.unit.health);
        systems.GetCombatSystemMut().setTeamTotalSpawned(1, m_teamB.count);
        systems.GetCombatSystemMut().setTeamMaxHP(1, m_teamB.count * m_teamB.unit.health);
    }

    void BattleConfigEditor::applyUnitParamsToEntities(Engine::ECS::ECSContext &ecs)
    {
        for (const auto &storePtr : ecs.stores.stores())
        {
            if (!storePtr || storePtr->size() == 0)
                continue;

            if (!storePtr->hasHealth() || !storePtr->hasTeam())
                continue;

            size_t n = storePtr->size();
            const auto &teams = storePtr->teams();

            for (size_t i = 0; i < n; ++i)
            {
                const UnitParams &u = (teams[i].id == 0) ? m_teamA.unit : m_teamB.unit;

                storePtr->healths()[i].value = u.health;

                if (storePtr->hasMoveSpeed())
                    storePtr->moveSpeeds()[i].value = u.moveSpeed;

                if (storePtr->hasRadius())
                    storePtr->radii()[i].r = u.radius;

                if (storePtr->hasSeparation())
                    storePtr->separations()[i].value = u.separation;

                if (storePtr->hasAttackCooldown())
                    storePtr->attackCooldowns()[i].interval = u.attackInterval;
            }
        }
    }

    void BattleConfigEditor::respawnFromPath(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems, const std::string &scenarioPath)
    {
        clearAllEntities(ecs);

        systems.ResetForRestart(ecs);
        systems.Initialize(ecs);

        Sample::SpawnFromScenarioFile(ecs, scenarioPath, false);

        applyToSystems(systems);
        applyUnitParamsToEntities(ecs);

        systems.GetCombatSystemMut().setHumanTeam(0);
    }

    void BattleConfigEditor::respawnWorkingCopy(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems)
    {
        writeWorkingCopy(m_workingCopyPath);
        respawnFromPath(ecs, systems, m_workingCopyPath);
    }

    void BattleConfigEditor::resetGame(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems)
    {
        m_combat = m_originalCombat;
        m_teamA = m_originalTeamA;
        m_teamB = m_originalTeamB;

        respawnFromPath(ecs, systems, m_battleConfigPath);
    }

    void BattleConfigEditor::reloadFromDisk(Engine::ECS::ECSContext &ecs, Sample::SystemRunner &systems)
    {
        loadFromFile(m_battleConfigPath);
        loadUnitConfig(m_unitConfigPath);

        respawnFromPath(ecs, systems, m_battleConfigPath);

        m_statusMsg = "Auto-reloaded from disk";
        m_statusTimer = 3.0f;
    }

    void BattleConfigEditor::clearAllEntities(Engine::ECS::ECSContext &ecs)
    {
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

} // namespace Editor
