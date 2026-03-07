#include "editor/BattleConfigEditor.h"
#include "update.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>

using json = nlohmann::json;

namespace Sample
{

BattleConfigEditor::BattleConfigEditor() = default;

// ---------------------------------------------------------------------------
// Load from JSON on disk
// ---------------------------------------------------------------------------
void BattleConfigEditor::loadFromFile(const std::string &path)
{
    m_filePath = path;

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

    // --- Combat ---
    if (root.contains("combat") && root["combat"].is_object())
    {
        const auto &c = root["combat"];
        auto g = [&](const char *k, float def) { return c.contains(k) ? c[k].get<float>() : def; };
        m_combat.meleeRange      = g("meleeRange",      CombatTuning::MELEE_RANGE_DEFAULT);
        m_combat.engageRange     = g("engageRange",     CombatTuning::ENGAGE_RANGE_DEFAULT);
        m_combat.damageMin       = g("damageMin",       CombatTuning::DAMAGE_MIN_DEFAULT);
        m_combat.damageMax       = g("damageMax",       CombatTuning::DAMAGE_MAX_DEFAULT);
        m_combat.deathRemoveDelay= g("deathRemoveDelay",CombatTuning::DEATH_REMOVE_DELAY_DEFAULT);
        m_combat.maxHPPerUnit    = g("maxHPPerUnit",    CombatTuning::MAX_HP_PER_UNIT_DEFAULT);
        m_combat.missChance      = g("missChance",      CombatTuning::MISS_CHANCE_DEFAULT);
        m_combat.critChance      = g("critChance",      CombatTuning::CRIT_CHANCE_DEFAULT);
        m_combat.critMultiplier  = g("critMultiplier",  CombatTuning::CRIT_MULTIPLIER_DEFAULT);
        m_combat.rageMaxBonus    = g("rageMaxBonus",    CombatTuning::RAGE_MAX_BONUS_DEFAULT);
        m_combat.cooldownJitter  = g("cooldownJitter",  CombatTuning::COOLDOWN_JITTER_DEFAULT);
        m_combat.staggerMax      = g("staggerMax",      CombatTuning::STAGGER_MAX_DEFAULT);

        // Legacy single-value fallback
        if (c.contains("damagePerHit") && !c.contains("damageMin"))
        {
            float d = c["damagePerHit"].get<float>();
            m_combat.damageMin = d * 0.6f;
            m_combat.damageMax = d * 1.4f;
        }
    }

    // --- Anchors ---
    m_anchors.clear();
    if (root.contains("anchors") && root["anchors"].is_object())
    {
        for (auto it = root["anchors"].begin(); it != root["anchors"].end(); ++it)
        {
            Anchor a;
            a.name = it.key();
            a.x = it.value().value("x", 0.0f);
            a.z = it.value().value("z", 0.0f);
            m_anchors.push_back(a);
        }
    }

    // --- Spawn Groups ---
    m_spawnGroups.clear();
    if (root.contains("spawnGroups") && root["spawnGroups"].is_array())
    {
        for (const auto &sg : root["spawnGroups"])
        {
            SpawnGroupUI g;
            g.id              = sg.value("id", std::string(""));
            g.unitType        = sg.value("unitType", std::string("CombatKnight"));
            g.count           = sg.value("count", 0);
            g.anchorName      = sg.value("anchor", std::string(""));
            g.team            = sg.value("team", 0);
            g.facingYawDeg    = sg.value("facingYawDeg", 0.0f);

            if (sg.contains("offset") && sg["offset"].is_object())
            {
                g.offsetX = sg["offset"].value("x", 0.0f);
                g.offsetZ = sg["offset"].value("z", 0.0f);
            }

            if (sg.contains("formation") && sg["formation"].is_object())
            {
                const auto &f = sg["formation"];
                g.formationKind = f.value("kind", std::string("grid"));
                g.columns       = f.value("columns", 5);
                g.jitterM       = f.value("jitter_m", 0.2f);
                g.circleRadiusM = f.value("radius_m", 0.0f);

                if (f.contains("spacing_m"))
                {
                    if (f["spacing_m"].is_string() && f["spacing_m"].get<std::string>() == "auto")
                        g.spacingAuto = true;
                    else if (f["spacing_m"].is_number())
                    {
                        g.spacingAuto = false;
                        g.spacingM = f["spacing_m"].get<float>();
                    }
                }
            }
            m_spawnGroups.push_back(g);
        }
    }

    m_statusMsg = "Loaded " + path;
    m_statusTimer = 2.0f;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
void BattleConfigEditor::draw(Engine::ECS::ECSContext &ecs, SystemRunner &systems)
{
    // Tick status timer
    if (m_statusTimer > 0.0f)
    {
        float dt = ImGui::GetIO().DeltaTime;
        m_statusTimer -= dt;
        if (m_statusTimer < 0.0f)
            m_statusTimer = 0.0f;
    }

    ImGuiIO &io = ImGui::GetIO();

    // Dock to the right side of the screen, full height
    const float panelWidth = 380.0f;
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelWidth, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y));

    // Solid opaque background so the panel doesn't bleed into the game world
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.12f, 0.12f, 0.14f, 1.0f));

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove
                           | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoCollapse;

    ImGui::Begin("Battle Config Editor", nullptr, flags);
    ImGui::PopStyleColor();

    drawControlButtons(ecs, systems);
    ImGui::Separator();
    drawCombatSection();
    ImGui::Separator();
    drawAnchorsSection();
    ImGui::Separator();
    drawSpawnGroupsSection();

    // Status bar
    if (m_statusTimer > 0.0f && !m_statusMsg.empty())
    {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", m_statusMsg.c_str());
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Combat Section
// ---------------------------------------------------------------------------
void BattleConfigEditor::drawCombatSection()
{
    if (!ImGui::CollapsingHeader("Combat Tuning", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    ImGui::PushItemWidth(-140);

    ImGui::Text("Ranges");
    ImGui::SliderFloat("Melee Range",  &m_combat.meleeRange,  0.5f, 30.0f, "%.1f");
    ImGui::SliderFloat("Engage Range", &m_combat.engageRange, 1.0f, 60.0f, "%.1f");

    ImGui::Spacing();
    ImGui::Text("Damage");
    ImGui::SliderFloat("Damage Min",   &m_combat.damageMin,       1.0f, 100.0f, "%.1f");
    ImGui::SliderFloat("Damage Max",   &m_combat.damageMax,       1.0f, 200.0f, "%.1f");
    ImGui::SliderFloat("Max HP / Unit",&m_combat.maxHPPerUnit,    10.0f, 1000.0f, "%.0f");

    ImGui::Spacing();
    ImGui::Text("Chances");
    ImGui::SliderFloat("Miss Chance",  &m_combat.missChance,      0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Crit Chance",  &m_combat.critChance,      0.0f, 1.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::SliderFloat("Crit Multiplier", &m_combat.critMultiplier, 1.0f, 5.0f, "%.2f");

    ImGui::Spacing();
    ImGui::Text("Timing & Modifiers");
    ImGui::SliderFloat("Death Remove Delay", &m_combat.deathRemoveDelay, 0.0f, 15.0f, "%.1f s");
    ImGui::SliderFloat("Rage Max Bonus",     &m_combat.rageMaxBonus,     0.0f, 1.0f,  "%.0f%%");
    ImGui::SliderFloat("Cooldown Jitter",    &m_combat.cooldownJitter,   0.0f, 1.0f,  "%.0f%%");
    ImGui::SliderFloat("Stagger Max",        &m_combat.staggerMax,       0.0f, 2.0f,  "%.2f s");

    ImGui::PopItemWidth();
}

// ---------------------------------------------------------------------------
// Anchors Section
// ---------------------------------------------------------------------------
void BattleConfigEditor::drawAnchorsSection()
{
    if (!ImGui::CollapsingHeader("Anchors (Spawn Origins)"))
        return;

    ImGui::PushItemWidth(-140);

    for (size_t i = 0; i < m_anchors.size(); ++i)
    {
        auto &a = m_anchors[i];
        ImGui::PushID(static_cast<int>(i));

        ImGui::Text("%s", a.name.c_str());
        ImGui::DragFloat("X", &a.x, 0.5f, -300.0f, 300.0f, "%.1f");
        ImGui::SameLine();
        ImGui::DragFloat("Z", &a.z, 0.5f, -300.0f, 300.0f, "%.1f");

        ImGui::PopID();
        if (i + 1 < m_anchors.size())
            ImGui::Spacing();
    }

    ImGui::PopItemWidth();
}

// ---------------------------------------------------------------------------
// Spawn Groups Section
// ---------------------------------------------------------------------------
void BattleConfigEditor::drawSpawnGroupsSection()
{
    if (!ImGui::CollapsingHeader("Spawn Groups"))
        return;

    ImGui::PushItemWidth(-140);

    for (size_t i = 0; i < m_spawnGroups.size(); ++i)
    {
        auto &sg = m_spawnGroups[i];
        ImGui::PushID(static_cast<int>(i));

        char label[128];
        snprintf(label, sizeof(label), "%s (Team %d)###sg%zu", sg.id.c_str(), sg.team, i);
        if (ImGui::TreeNode(label))
        {
            // Unit type (read-only display)
            ImGui::Text("Unit Type: %s", sg.unitType.c_str());

            ImGui::SliderInt("Count", &sg.count, 1, 100);
            ImGui::SliderInt("Team",  &sg.team,  0, 3);
            ImGui::SliderFloat("Facing (deg)", &sg.facingYawDeg, -180.0f, 180.0f, "%.1f");

            ImGui::Text("Anchor: %s", sg.anchorName.c_str());
            ImGui::DragFloat("Offset X", &sg.offsetX, 0.5f, -200.0f, 200.0f, "%.1f");
            ImGui::DragFloat("Offset Z", &sg.offsetZ, 0.5f, -200.0f, 200.0f, "%.1f");

            if (ImGui::TreeNode("Formation"))
            {
                // Kind selector
                const char *kinds[] = {"grid", "circle"};
                int kindIdx = (sg.formationKind == "circle") ? 1 : 0;
                if (ImGui::Combo("Kind", &kindIdx, kinds, 2))
                    sg.formationKind = kinds[kindIdx];

                if (kindIdx == 0) // grid
                {
                    ImGui::SliderInt("Columns", &sg.columns, 1, 20);
                }
                else // circle
                {
                    ImGui::DragFloat("Circle Radius", &sg.circleRadiusM, 0.5f, 1.0f, 100.0f, "%.1f m");
                }

                ImGui::Checkbox("Auto Spacing", &sg.spacingAuto);
                if (!sg.spacingAuto)
                {
                    ImGui::DragFloat("Spacing", &sg.spacingM, 0.1f, 0.5f, 30.0f, "%.1f m");
                }

                ImGui::DragFloat("Jitter", &sg.jitterM, 0.05f, 0.0f, 5.0f, "%.2f m");

                ImGui::TreePop();
            }

            ImGui::TreePop();
        }

        ImGui::PopID();
    }

    ImGui::PopItemWidth();
}

// ---------------------------------------------------------------------------
// Control Buttons
// ---------------------------------------------------------------------------
void BattleConfigEditor::drawControlButtons(Engine::ECS::ECSContext &ecs, SystemRunner &systems)
{
    // Apply — pushes combat config to running systems (instant for combat params)
    if (ImGui::Button("Apply Changes"))
    {
        applyToSystems(systems);
        m_statusMsg = "Combat config applied (live)";
        m_statusTimer = 2.0f;
    }

    ImGui::SameLine();

    // Restart — clears entities, re-spawns from original file, applies editor combat config, starts
    if (ImGui::Button("Restart Battle"))
    {
        restartBattle(ecs, systems);
        m_statusMsg = "Battle restarted with current settings";
        m_statusTimer = 3.0f;
    }
}

// ---------------------------------------------------------------------------
// Apply combat config to running systems (instant for poll-based params)
// ---------------------------------------------------------------------------
void BattleConfigEditor::applyToSystems(SystemRunner &systems)
{
    systems.GetCombatSystemMut().applyConfig(m_combat);
}

// ---------------------------------------------------------------------------
// Restart: clear entities safely, re-spawn from original file, apply config
// ---------------------------------------------------------------------------
void BattleConfigEditor::restartBattle(Engine::ECS::ECSContext &ecs, SystemRunner &systems)
{
    // 1) Clear all entity data from stores (keeps component registry, prefabs, archetypes intact).
    clearAllEntities(ecs);

    // 2) Reset combat system battle state (death queues, flags, unit memories)
    //    and allow SystemRunner to re-initialize query IDs.
    systems.ResetForRestart();
    systems.Initialize(ecs);

    // 3) Re-spawn everything from the original untouched config file.
    SpawnFromScenarioFile(ecs, m_filePath, /*selectSpawned=*/false);

    // 4) Apply the editor's current combat tuning on top and start.
    applyToSystems(systems);
    systems.GetCombatSystemMut().setHumanTeam(0);
    systems.GetCombatSystemMut().startBattle();
}

// ---------------------------------------------------------------------------
// Clear every entity from every archetype store (safe, keeps ECS metadata)
// ---------------------------------------------------------------------------
void BattleConfigEditor::clearAllEntities(Engine::ECS::ECSContext &ecs)
{
    for (const auto &storePtr : ecs.stores.stores())
    {
        if (!storePtr)
            continue;

        // Destroy entity records for every entity in this store.
        while (storePtr->size() > 0)
        {
            Engine::ECS::Entity e = storePtr->entities()[storePtr->size() - 1];
            ecs.entities.destroy(e);
            storePtr->destroyRow(storePtr->size() - 1);
        }
    }
}

} // namespace Sample
