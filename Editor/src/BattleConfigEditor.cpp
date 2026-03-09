#include "editor/BattleConfigEditor.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using json = nlohmann::json;

namespace
{
    static void ensureObject(json &root, const char *k)
    {
        if (!root.contains(k) || !root[k].is_object())
            root[k] = json::object();
    }

    static void ensureArray(json &root, const char *k)
    {
        if (!root.contains(k) || !root[k].is_array())
            root[k] = json::array();
    }

    static void ensureXZObject(json &o)
    {
        if (!o.is_object())
            o = json::object();
        if (!o.contains("x") || !o["x"].is_number())
            o["x"] = 0.0f;
        if (!o.contains("z") || !o["z"].is_number())
            o["z"] = 0.0f;
    }

    static bool inputFloatClamped(const char *label, float *v, float minV, float maxV)
    {
        if (!ImGui::InputFloat(label, v, 0.0f, 0.0f, "%.3f"))
            return false;
        if (*v < minV)
            *v = minV;
        if (*v > maxV)
            *v = maxV;
        return true;
    }
}

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

        m_doc = std::move(root);
        ensureObject(m_doc, "combat");
        ensureObject(m_doc, "anchors");
        ensureArray(m_doc, "spawnGroups");
        ensureArray(m_doc, "obstacles");

        m_originalDoc = m_doc;
        m_selectedAnchorKey.clear();
        m_selectedSpawnGroupIndex = -1;
        m_selectedObstacleIndex = -1;
        m_selectedGapIndex = -1;

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

    void BattleConfigEditor::draw(Engine::ECS::ECSContext &ecs)
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

                if (changed)
                    reloadFromDisk(ecs);
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
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - panelWidth, 0.0f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(panelWidth, io.DisplaySize.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.78f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse;

        ImGui::Begin("Editor: GameWorld", nullptr, flags);

        drawControlButtons(ecs);
        ImGui::Separator();
        drawCombatSection();
        ImGui::Separator();
        drawAnchorsSection();
        ImGui::Separator();
        drawSpawnGroupsSection();
        ImGui::Separator();
        drawObstaclesSection();

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

        ensureObject(m_doc, "combat");
        auto &c = m_doc["combat"];

        ImGui::PushItemWidth(-120);

        auto editFloat = [&](const char *label, const char *k, float def, float minV, float maxV)
        {
            float v = c.contains(k) && c[k].is_number() ? c[k].get<float>() : def;
            if (inputFloatClamped(label, &v, minV, maxV))
                c[k] = v;
        };

        ImGui::Text("Ranges");
        editFloat("Melee Range", "meleeRange", 2.0f, 0.0f, 9999.0f);
        editFloat("Engage Range", "engageRange", 10.0f, 0.0f, 9999.0f);

        ImGui::Spacing();
        ImGui::Text("Damage");
        editFloat("Damage Min", "damageMin", 12.0f, 0.0f, 9999.0f);
        editFloat("Damage Max", "damageMax", 28.0f, 0.0f, 9999.0f);
        editFloat("Max HP / Unit", "maxHPPerUnit", 140.0f, 0.0f, 999999.0f);

        ImGui::Spacing();
        ImGui::Text("Chances");
        editFloat("Miss Chance", "missChance", 0.20f, 0.0f, 1.0f);
        editFloat("Crit Chance", "critChance", 0.10f, 0.0f, 1.0f);
        editFloat("Crit Multiplier", "critMultiplier", 2.0f, 0.0f, 1000.0f);

        ImGui::Spacing();
        ImGui::Text("Timing & Modifiers");
        editFloat("Death Remove Delay", "deathRemoveDelay", 3.0f, 0.0f, 9999.0f);
        editFloat("Rage Max Bonus", "rageMaxBonus", 0.50f, 0.0f, 1.0f);
        editFloat("Cooldown Jitter", "cooldownJitter", 0.30f, 0.0f, 1.0f);
        editFloat("Stagger Max", "staggerMax", 0.60f, 0.0f, 9999.0f);

        ImGui::PopItemWidth();
    }

    void BattleConfigEditor::drawAnchorsSection()
    {
        if (!ImGui::CollapsingHeader("Anchors", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ensureObject(m_doc, "anchors");
        auto &anchors = m_doc["anchors"];

        if (m_newAnchorName[0] == '\0')
            std::snprintf(m_newAnchorName, sizeof(m_newAnchorName), "%s", "new_anchor");

        ImGui::InputText("New Anchor Name", m_newAnchorName, sizeof(m_newAnchorName));
        ImGui::SameLine();
        if (ImGui::Button("Add Anchor"))
        {
            const std::string key = std::string(m_newAnchorName);
            if (!key.empty())
            {
                if (!anchors.contains(key))
                    anchors[key] = json::object();
                ensureXZObject(anchors[key]);
                m_selectedAnchorKey = key;
                std::snprintf(m_renameAnchorName, sizeof(m_renameAnchorName), "%s", key.c_str());
            }
        }

        ImGui::Columns(2, "##AnchorsColumns", true);
        ImGui::BeginChild("##AnchorsList", ImVec2(180.0f, 160.0f), true);
        for (auto it = anchors.begin(); it != anchors.end(); ++it)
        {
            const std::string key = it.key();
            const bool selected = (key == m_selectedAnchorKey);
            if (ImGui::Selectable(key.c_str(), selected))
            {
                m_selectedAnchorKey = key;
                std::snprintf(m_renameAnchorName, sizeof(m_renameAnchorName), "%s", key.c_str());
            }
        }
        ImGui::EndChild();
        ImGui::NextColumn();

        if (!m_selectedAnchorKey.empty() && anchors.contains(m_selectedAnchorKey))
        {
            auto &a = anchors[m_selectedAnchorKey];
            ensureXZObject(a);

            ImGui::InputText("Name", m_renameAnchorName, sizeof(m_renameAnchorName));
            ImGui::SameLine();
            if (ImGui::Button("Rename"))
            {
                const std::string newKey = std::string(m_renameAnchorName);
                if (!newKey.empty() && newKey != m_selectedAnchorKey)
                {
                    if (!anchors.contains(newKey))
                    {
                        anchors[newKey] = a;
                        anchors.erase(m_selectedAnchorKey);
                        m_selectedAnchorKey = newKey;
                    }
                    else
                    {
                        m_statusMsg = "Anchor name already exists";
                        m_statusTimer = 3.0f;
                        std::snprintf(m_renameAnchorName, sizeof(m_renameAnchorName), "%s", m_selectedAnchorKey.c_str());
                    }
                }
            }

            float x = a.value("x", 0.0f);
            float z = a.value("z", 0.0f);
            ImGui::PushItemWidth(120.0f);
            if (ImGui::InputFloat("X", &x, 0.0f, 0.0f, "%.3f"))
                a["x"] = x;
            if (ImGui::InputFloat("Z", &z, 0.0f, 0.0f, "%.3f"))
                a["z"] = z;
            ImGui::PopItemWidth();

            ImGui::Spacing();
            if (ImGui::Button("Delete Anchor"))
            {
                anchors.erase(m_selectedAnchorKey);
                m_selectedAnchorKey.clear();
                m_selectedSpawnGroupIndex = -1;
            }
        }
        else
        {
            ImGui::TextDisabled("Select an anchor to edit");
        }

        ImGui::Columns(1);
    }

    void BattleConfigEditor::drawSpawnGroupsSection()
    {
        if (!ImGui::CollapsingHeader("Spawn Groups", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ensureArray(m_doc, "spawnGroups");
        auto &groups = m_doc["spawnGroups"];

        if (m_newSpawnGroupId[0] == '\0')
            std::snprintf(m_newSpawnGroupId, sizeof(m_newSpawnGroupId), "%s", "spawn_group");

        ImGui::InputText("New Group Id", m_newSpawnGroupId, sizeof(m_newSpawnGroupId));
        ImGui::SameLine();
        if (ImGui::Button("Add Spawn Group"))
        {
            json g = json::object();
            g["id"] = std::string(m_newSpawnGroupId);
            g["unitType"] = "CombatKnight";
            g["team"] = 0;
            g["count"] = 20;
            g["anchor"] = "";
            g["offset"] = json::object({{"x", 0.0f}, {"z", 0.0f}});
            g["facingYawDeg"] = 0.0f;
            g["formation"] = json::object({{"kind", "grid"}, {"columns", 5}, {"spacing_m", 5.0f}, {"jitter_m", 0.2f}});

            groups.push_back(g);
            m_selectedSpawnGroupIndex = static_cast<int>(groups.size()) - 1;
        }

        ImGui::Columns(2, "##SpawnGroupsColumns", true);
        ImGui::BeginChild("##SpawnGroupsList", ImVec2(220.0f, 190.0f), true);
        for (int i = 0; i < static_cast<int>(groups.size()); ++i)
        {
            const auto &g = groups[i];
            std::string label = "[" + std::to_string(i) + "] ";
            label += g.value("id", std::string("(no-id)"));
            const bool selected = (i == m_selectedSpawnGroupIndex);
            if (ImGui::Selectable(label.c_str(), selected))
                m_selectedSpawnGroupIndex = i;
        }
        ImGui::EndChild();
        ImGui::NextColumn();

        if (m_selectedSpawnGroupIndex >= 0 && m_selectedSpawnGroupIndex < static_cast<int>(groups.size()))
        {
            auto &g = groups[m_selectedSpawnGroupIndex];
            if (!g.is_object())
                g = json::object();

            char idBuf[64] = {0};
            char unitBuf[64] = {0};
            std::snprintf(idBuf, sizeof(idBuf), "%s", g.value("id", std::string("")).c_str());
            std::snprintf(unitBuf, sizeof(unitBuf), "%s", g.value("unitType", std::string("")).c_str());

            if (ImGui::InputText("Id", idBuf, sizeof(idBuf)))
                g["id"] = std::string(idBuf);
            if (ImGui::InputText("Unit Type", unitBuf, sizeof(unitBuf)))
                g["unitType"] = std::string(unitBuf);

            int team = g.value("team", 0);
            if (ImGui::InputInt("Team", &team))
            {
                if (team < 0)
                    team = 0;
                if (team > 255)
                    team = 255;
                g["team"] = team;
            }

            int count = g.value("count", 0);
            if (ImGui::InputInt("Count", &count))
            {
                if (count < 0)
                    count = 0;
                g["count"] = count;
            }

            // Anchor selection
            ensureObject(m_doc, "anchors");
            auto &anchors = m_doc["anchors"];
            std::string anchorName = g.value("anchor", std::string(""));
            const char *anchorPreview = anchorName.empty() ? "(none)" : anchorName.c_str();
            if (ImGui::BeginCombo("Anchor", anchorPreview))
            {
                if (ImGui::Selectable("(none)", anchorName.empty()))
                {
                    g["anchor"] = "";
                    anchorName.clear();
                }
                for (auto it = anchors.begin(); it != anchors.end(); ++it)
                {
                    const std::string key = it.key();
                    const bool selected = (key == anchorName);
                    if (ImGui::Selectable(key.c_str(), selected))
                    {
                        g["anchor"] = key;
                        anchorName = key;
                    }
                    if (selected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            // Offset
            if (!g.contains("offset") || !g["offset"].is_object())
                g["offset"] = json::object();
            ensureXZObject(g["offset"]);
            float offX = g["offset"].value("x", 0.0f);
            float offZ = g["offset"].value("z", 0.0f);
            if (ImGui::InputFloat("Offset X", &offX, 0.0f, 0.0f, "%.3f"))
                g["offset"]["x"] = offX;
            if (ImGui::InputFloat("Offset Z", &offZ, 0.0f, 0.0f, "%.3f"))
                g["offset"]["z"] = offZ;

            float yaw = g.value("facingYawDeg", 0.0f);
            if (ImGui::InputFloat("Facing Yaw Deg", &yaw, 0.0f, 0.0f, "%.3f"))
                g["facingYawDeg"] = yaw;

            // Formation
            if (!g.contains("formation") || !g["formation"].is_object())
                g["formation"] = json::object();
            auto &f = g["formation"];
            const std::string kind = f.value("kind", std::string("grid"));
            int kindIndex = (kind == "circle") ? 1 : 0;
            const char *kinds[] = {"grid", "circle"};
            if (ImGui::Combo("Formation", &kindIndex, kinds, 2))
                f["kind"] = std::string(kinds[kindIndex]);

            if (std::string(f.value("kind", std::string("grid"))) == "circle")
            {
                float radiusM = f.value("radius_m", 0.0f);
                if (ImGui::InputFloat("Radius (m)", &radiusM, 0.0f, 0.0f, "%.3f"))
                    f["radius_m"] = radiusM;
            }
            else
            {
                int columns = f.value("columns", 0);
                if (ImGui::InputInt("Columns", &columns))
                {
                    if (columns < 0)
                        columns = 0;
                    f["columns"] = columns;
                }
            }

            bool spacingAuto = false;
            if (f.contains("spacing_m") && f["spacing_m"].is_string() && f["spacing_m"].get<std::string>() == "auto")
                spacingAuto = true;
            if (!f.contains("spacing_m"))
                f["spacing_m"] = 5.0f;

            if (ImGui::Checkbox("Auto Spacing", &spacingAuto))
            {
                if (spacingAuto)
                    f["spacing_m"] = "auto";
                else
                    f["spacing_m"] = 5.0f;
            }

            if (!spacingAuto)
            {
                float spacingM = f["spacing_m"].is_number() ? f["spacing_m"].get<float>() : 5.0f;
                if (ImGui::InputFloat("Spacing (m)", &spacingM, 0.0f, 0.0f, "%.3f"))
                    f["spacing_m"] = spacingM;
            }

            float jitterM = f.value("jitter_m", 0.0f);
            if (ImGui::InputFloat("Jitter (m)", &jitterM, 0.0f, 0.0f, "%.3f"))
                f["jitter_m"] = jitterM;

            ImGui::Spacing();
            if (ImGui::Button("Delete Spawn Group"))
            {
                groups.erase(groups.begin() + m_selectedSpawnGroupIndex);
                if (m_selectedSpawnGroupIndex >= static_cast<int>(groups.size()))
                    m_selectedSpawnGroupIndex = static_cast<int>(groups.size()) - 1;
            }
        }
        else
        {
            ImGui::TextDisabled("Select a spawn group to edit");
        }

        ImGui::Columns(1);
    }

    void BattleConfigEditor::drawObstaclesSection()
    {
        if (!ImGui::CollapsingHeader("Obstacles", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        ensureArray(m_doc, "obstacles");
        auto &obstacles = m_doc["obstacles"];

        if (m_newObstaclePrefab[0] == '\0')
            std::snprintf(m_newObstaclePrefab, sizeof(m_newObstaclePrefab), "%s", "TreeStumpWall");

        ImGui::InputText("New Obstacle Prefab", m_newObstaclePrefab, sizeof(m_newObstaclePrefab));
        ImGui::SameLine();
        if (ImGui::Button("Add Obstacle"))
        {
            json o = json::object();
            o["prefab"] = std::string(m_newObstaclePrefab);
            o["start"] = json::object({{"x", 0.0f}, {"z", 0.0f}});
            o["end"] = json::object({{"x", 0.0f}, {"z", 10.0f}});
            o["spacing"] = 3.0f;
            o["gaps"] = json::array();
            obstacles.push_back(o);
            m_selectedObstacleIndex = static_cast<int>(obstacles.size()) - 1;
            m_selectedGapIndex = -1;
        }

        ImGui::Columns(2, "##ObstaclesColumns", true);
        ImGui::BeginChild("##ObstaclesList", ImVec2(220.0f, 190.0f), true);
        for (int i = 0; i < static_cast<int>(obstacles.size()); ++i)
        {
            const auto &o = obstacles[i];
            std::string label = "[" + std::to_string(i) + "] ";
            label += o.value("prefab", std::string("(no-prefab)"));
            const bool selected = (i == m_selectedObstacleIndex);
            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedObstacleIndex = i;
                m_selectedGapIndex = -1;
            }
        }
        ImGui::EndChild();
        ImGui::NextColumn();

        if (m_selectedObstacleIndex >= 0 && m_selectedObstacleIndex < static_cast<int>(obstacles.size()))
        {
            auto &o = obstacles[m_selectedObstacleIndex];
            if (!o.is_object())
                o = json::object();

            char prefabBuf[64] = {0};
            std::snprintf(prefabBuf, sizeof(prefabBuf), "%s", o.value("prefab", std::string("")).c_str());
            if (ImGui::InputText("Prefab", prefabBuf, sizeof(prefabBuf)))
                o["prefab"] = std::string(prefabBuf);

            if (!o.contains("start") || !o["start"].is_object())
                o["start"] = json::object();
            if (!o.contains("end") || !o["end"].is_object())
                o["end"] = json::object();
            ensureXZObject(o["start"]);
            ensureXZObject(o["end"]);

            float sx = o["start"].value("x", 0.0f);
            float sz = o["start"].value("z", 0.0f);
            float ex = o["end"].value("x", 0.0f);
            float ez = o["end"].value("z", 0.0f);

            ImGui::Text("Start");
            if (ImGui::InputFloat("Start X", &sx, 0.0f, 0.0f, "%.3f"))
                o["start"]["x"] = sx;
            if (ImGui::InputFloat("Start Z", &sz, 0.0f, 0.0f, "%.3f"))
                o["start"]["z"] = sz;

            ImGui::Text("End");
            if (ImGui::InputFloat("End X", &ex, 0.0f, 0.0f, "%.3f"))
                o["end"]["x"] = ex;
            if (ImGui::InputFloat("End Z", &ez, 0.0f, 0.0f, "%.3f"))
                o["end"]["z"] = ez;

            float spacing = o.value("spacing", 3.0f);
            if (ImGui::InputFloat("Spacing", &spacing, 0.0f, 0.0f, "%.3f"))
            {
                if (spacing < 0.1f)
                    spacing = 0.1f;
                o["spacing"] = spacing;
            }

            // Gaps
            if (!o.contains("gaps") || !o["gaps"].is_array())
                o["gaps"] = json::array();
            auto &gaps = o["gaps"];

            ImGui::Separator();
            ImGui::Text("Gaps");
            if (ImGui::Button("Add Gap"))
            {
                json g = json::object();
                g["center"] = json::object({{"x", 0.0f}, {"z", 0.0f}});
                g["width"] = 10.0f;
                gaps.push_back(g);
                m_selectedGapIndex = static_cast<int>(gaps.size()) - 1;
            }

            ImGui::BeginChild("##GapsList", ImVec2(0.0f, 90.0f), true);
            for (int gi = 0; gi < static_cast<int>(gaps.size()); ++gi)
            {
                std::string glabel = "Gap [" + std::to_string(gi) + "]";
                const bool selected = (gi == m_selectedGapIndex);
                if (ImGui::Selectable(glabel.c_str(), selected))
                    m_selectedGapIndex = gi;
            }
            ImGui::EndChild();

            if (m_selectedGapIndex >= 0 && m_selectedGapIndex < static_cast<int>(gaps.size()))
            {
                auto &g = gaps[m_selectedGapIndex];
                if (!g.is_object())
                    g = json::object();
                if (!g.contains("center") || !g["center"].is_object())
                    g["center"] = json::object();
                ensureXZObject(g["center"]);

                float cx = g["center"].value("x", 0.0f);
                float cz = g["center"].value("z", 0.0f);
                float w = g.value("width", 0.0f);
                if (ImGui::InputFloat("Center X", &cx, 0.0f, 0.0f, "%.3f"))
                    g["center"]["x"] = cx;
                if (ImGui::InputFloat("Center Z", &cz, 0.0f, 0.0f, "%.3f"))
                    g["center"]["z"] = cz;
                if (ImGui::InputFloat("Width", &w, 0.0f, 0.0f, "%.3f"))
                {
                    if (w < 0.0f)
                        w = 0.0f;
                    g["width"] = w;
                }

                if (ImGui::Button("Delete Gap"))
                {
                    gaps.erase(gaps.begin() + m_selectedGapIndex);
                    if (m_selectedGapIndex >= static_cast<int>(gaps.size()))
                        m_selectedGapIndex = static_cast<int>(gaps.size()) - 1;
                }
            }

            ImGui::Spacing();
            if (ImGui::Button("Delete Obstacle"))
            {
                obstacles.erase(obstacles.begin() + m_selectedObstacleIndex);
                if (m_selectedObstacleIndex >= static_cast<int>(obstacles.size()))
                    m_selectedObstacleIndex = static_cast<int>(obstacles.size()) - 1;
                m_selectedGapIndex = -1;
            }
        }
        else
        {
            ImGui::TextDisabled("Select an obstacle to edit");
        }

        ImGui::Columns(1);
    }

    void BattleConfigEditor::writeWorkingCopy(const std::string &outPath)
    {
        std::ofstream outFile(outPath);
        if (outFile.is_open())
            outFile << m_doc.dump(4) << std::endl;
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

        // Update snapshots to match what we just saved.
        m_originalDoc = m_doc;

        return true;
    }

    void BattleConfigEditor::drawControlButtons(Engine::ECS::ECSContext &ecs)
    {
        if (ImGui::Button("Apply (Preview)"))
        {
            respawnWorkingCopy(ecs);
            m_statusMsg = "Preview applied (respawned from working copy)";
            m_statusTimer = 3.0f;
        }

        ImGui::SameLine();

        if (ImGui::Button("Save to GameWorld.json"))
        {
            if (writePermanent())
            {
                respawnFromPath(ecs, m_battleConfigPath);
                m_statusMsg = "Saved to GameWorld.json";
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
            resetGame(ecs);
            m_statusMsg = "Reset to last loaded/saved config";
            m_statusTimer = 3.0f;
        }
    }

    void BattleConfigEditor::respawnFromPath(Engine::ECS::ECSContext &ecs, const std::string &scenarioPath)
    {
        clearAllEntities(ecs);

        Editor::SpawnFromGameWorldFile(ecs, scenarioPath, /*selectSpawned=*/false);
    }

    void BattleConfigEditor::respawnWorkingCopy(Engine::ECS::ECSContext &ecs)
    {
        writeWorkingCopy(m_workingCopyPath);
        respawnFromPath(ecs, m_workingCopyPath);
    }

    void BattleConfigEditor::resetGame(Engine::ECS::ECSContext &ecs)
    {
        m_doc = m_originalDoc;

        respawnFromPath(ecs, m_battleConfigPath);
    }

    void BattleConfigEditor::reloadFromDisk(Engine::ECS::ECSContext &ecs)
    {
        loadFromFile(m_battleConfigPath);

        respawnFromPath(ecs, m_battleConfigPath);

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
