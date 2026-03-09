#include "editor/EntityTypeEditor.h"

#include "ECS/ECSContext.h"
#include "ECS/Prefab.h"

#include "assets/AssetManager.h"

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace
{
    static bool endsWith(const std::string &s, const std::string &suffix)
    {
        if (suffix.size() > s.size())
            return false;
        return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin());
    }

    static std::string trim(std::string s)
    {
        auto isSpace = [](unsigned char c)
        { return std::isspace(c) != 0; };
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.front())))
            s.erase(s.begin());
        while (!s.empty() && isSpace(static_cast<unsigned char>(s.back())))
            s.pop_back();
        return s;
    }

    static bool validNameChar(char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
    }

    static std::string sanitizeFileStem(const std::string &name)
    {
        std::string out;
        out.reserve(name.size());
        for (char c : name)
        {
            if (validNameChar(c))
                out.push_back(c);
            else if (std::isspace(static_cast<unsigned char>(c)))
                out.push_back('_');
        }
        if (out.empty())
            out = "Entity";
        return out;
    }

    static void ensureDefaultsObject(nlohmann::json &doc)
    {
        if (!doc.contains("defaults") || !doc["defaults"].is_object())
            doc["defaults"] = nlohmann::json::object();
    }

    static void ensureComponentsArray(nlohmann::json &doc)
    {
        if (!doc.contains("components") || !doc["components"].is_array())
            doc["components"] = nlohmann::json::array();
    }

    static bool componentsContains(const nlohmann::json &doc, const std::string &comp)
    {
        if (!doc.contains("components") || !doc["components"].is_array())
            return false;
        for (const auto &v : doc["components"])
        {
            if (v.is_string() && v.get<std::string>() == comp)
                return true;
        }
        return false;
    }

    static void componentsAdd(nlohmann::json &doc, const std::string &comp)
    {
        ensureComponentsArray(doc);
        if (!componentsContains(doc, comp))
            doc["components"].push_back(comp);
    }

    static void componentsRemove(nlohmann::json &doc, const std::string &comp)
    {
        if (!doc.contains("components") || !doc["components"].is_array())
            return;
        auto &arr = doc["components"];
        nlohmann::json out = nlohmann::json::array();
        for (const auto &v : arr)
        {
            if (v.is_string() && v.get<std::string>() == comp)
                continue;
            out.push_back(v);
        }
        arr = out;
    }

    static float getFloatOr(const nlohmann::json &j, const char *k, float def)
    {
        if (j.contains(k) && j[k].is_number())
            return j[k].get<float>();
        return def;
    }

    static int getIntOr(const nlohmann::json &j, const char *k, int def)
    {
        if (j.contains(k) && j[k].is_number_integer())
            return j[k].get<int>();
        if (j.contains(k) && j[k].is_number())
            return static_cast<int>(j[k].get<float>());
        return def;
    }

    static void setIfEnabled(nlohmann::json &defaults, const std::string &comp, bool enabled)
    {
        if (enabled)
        {
            if (!defaults.contains(comp) || !defaults[comp].is_object())
                defaults[comp] = nlohmann::json::object();
        }
        else
        {
            if (defaults.contains(comp))
                defaults.erase(comp);
        }
    }

    static std::string fileNameFromPath(const std::string &p)
    {
        if (p.empty())
            return std::string();
        return std::filesystem::path(p).filename().string();
    }
}

namespace Editor
{

    EntityTypeEditor::EntityTypeEditor(std::string editorEntitiesDir)
        : m_editorEntitiesDir(std::move(editorEntitiesDir))
    {
        newEntity();
    }

    void EntityTypeEditor::refreshList()
    {
        m_entries.clear();

        auto collect = [&](const std::filesystem::path &dir, bool isEditorOwned)
        {
            if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir))
                return;

            for (const auto &entry : std::filesystem::directory_iterator(dir))
            {
                if (!entry.is_regular_file())
                    continue;
                const auto p = entry.path();
                if (p.extension() != ".json")
                    continue;

                const std::string name = p.stem().string();
                m_entries.push_back({name, p, isEditorOwned});
            }
        };

        collect("entities", /*isEditorOwned=*/false);
        collect(m_editorEntitiesDir, /*isEditorOwned=*/true);

        // Prefer editor-owned overrides first when names collide
        std::sort(m_entries.begin(), m_entries.end(), [](const EntityEntry &a, const EntityEntry &b)
                  {
                  if (a.name != b.name)
                      return a.name < b.name;
                  return (a.isEditorOwned && !b.isEditorOwned); });

        // De-dup: keep editor-owned when same stem exists
        std::vector<EntityEntry> dedup;
        dedup.reserve(m_entries.size());
        for (const auto &e : m_entries)
        {
            if (!dedup.empty() && dedup.back().name == e.name)
            {
                if (e.isEditorOwned)
                    dedup.back() = e;
                continue;
            }
            dedup.push_back(e);
        }
        m_entries = std::move(dedup);

        m_listDirty = false;
    }

    void EntityTypeEditor::loadSelected()
    {
        if (m_selectedIndex < 0 || m_selectedIndex >= static_cast<int>(m_entries.size()))
            return;

        const auto &e = m_entries[m_selectedIndex];
        std::ifstream f(e.path);
        if (!f.is_open())
        {
            m_statusMsg = "Failed to open " + e.path.string();
            m_statusTimer = 3.0f;
            return;
        }

        try
        {
            m_doc = nlohmann::json::parse(f);
        }
        catch (const std::exception &ex)
        {
            m_statusMsg = std::string("JSON parse error: ") + ex.what();
            m_statusTimer = 4.0f;
            return;
        }

        std::string nm = m_doc.value("name", e.name);
        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", nm.c_str());

        std::string model;
        if (m_doc.contains("visual") && m_doc["visual"].is_object())
            model = m_doc["visual"].value("model", std::string(""));
        std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", model.c_str());

        m_loadedFromPath = e.path.string();
        m_loadedFromEditor = e.isEditorOwned;

        m_statusMsg = "Loaded " + e.name;
        m_statusTimer = 2.0f;
    }

    void EntityTypeEditor::newEntity()
    {
        m_doc = nlohmann::json::object();
        m_doc["name"] = "NewEntity";
        m_doc["components"] = nlohmann::json::array();
        m_doc["defaults"] = nlohmann::json::object();

        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", "NewEntity");
        m_modelBuf[0] = '\0';
        m_customCompBuf[0] = '\0';

        m_loadedFromPath.clear();
        m_loadedFromEditor = false;
    }

    void EntityTypeEditor::refreshSModelList()
    {
        m_smodels.clear();

        const std::filesystem::path assetsRoot("assets");
        if (!std::filesystem::exists(assetsRoot) || !std::filesystem::is_directory(assetsRoot))
        {
            m_smodelsDirty = false;
            return;
        }

        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(assetsRoot, ec))
        {
            if (ec)
                break;
            if (!entry.is_regular_file())
                continue;

            const auto p = entry.path();
            if (p.extension() != ".smodel")
                continue;

            const std::string runtimePath = p.generic_string();
            m_smodels.push_back({p.filename().string(), runtimePath});
        }

        std::sort(m_smodels.begin(), m_smodels.end(), [](const SModelEntry &a, const SModelEntry &b)
                  { return a.fileName < b.fileName; });

        m_smodelsDirty = false;
    }

    nlohmann::json &EntityTypeEditor::defaultsObj()
    {
        ensureDefaultsObject(m_doc);
        return m_doc["defaults"];
    }

    const nlohmann::json &EntityTypeEditor::defaultsObj() const
    {
        static const nlohmann::json kEmpty = nlohmann::json::object();
        if (!m_doc.contains("defaults") || !m_doc["defaults"].is_object())
            return kEmpty;
        return m_doc["defaults"];
    }

    bool EntityTypeEditor::hasComponent(const std::string &comp) const
    {
        return componentsContains(m_doc, comp);
    }

    void EntityTypeEditor::ensureComponent(const std::string &comp, bool enabled)
    {
        ensureComponentsArray(m_doc);
        if (enabled)
        {
            componentsAdd(m_doc, comp);
            setIfEnabled(m_doc["defaults"], comp, true);
        }
        else
        {
            componentsRemove(m_doc, comp);
            setIfEnabled(m_doc["defaults"], comp, false);
        }
    }

    bool EntityTypeEditor::saveToEditorEntities(std::string *outError)
    {
        std::string name = trim(std::string(m_nameBuf));
        if (name.empty())
        {
            if (outError)
                *outError = "Name is required";
            return false;
        }

        // Write canonical name into JSON
        m_doc["name"] = name;

        // Visual
        std::string model = trim(std::string(m_modelBuf));
        if (!model.empty())
        {
            m_doc["visual"] = nlohmann::json::object();
            m_doc["visual"]["model"] = model;
        }
        else
        {
            if (m_doc.contains("visual"))
                m_doc.erase("visual");
        }

        std::filesystem::path dir(m_editorEntitiesDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);

        const std::filesystem::path outPath = dir / (sanitizeFileStem(name) + ".json");

        std::ofstream o(outPath);
        if (!o.is_open())
        {
            if (outError)
                *outError = "Failed to write to " + outPath.string();
            return false;
        }

        o << m_doc.dump(4) << std::endl;

        m_loadedFromPath = outPath.string();
        m_loadedFromEditor = true;

        m_listDirty = true;

        return true;
    }

    bool EntityTypeEditor::deleteEditorEntity(std::string *outError)
    {
        if (!m_loadedFromEditor || m_loadedFromPath.empty())
        {
            if (outError)
                *outError = "Selected entity is not editor-owned";
            return false;
        }

        std::error_code ec;
        const bool ok = std::filesystem::remove(std::filesystem::path(m_loadedFromPath), ec);
        if (!ok || ec)
        {
            if (outError)
                *outError = "Failed to delete file";
            return false;
        }

        m_statusMsg = "Deleted editor override";
        m_statusTimer = 2.0f;

        m_listDirty = true;
        return true;
    }

    void EntityTypeEditor::drawEntityList()
    {
        if (ImGui::Button("Refresh"))
            m_listDirty = true;

        ImGui::SameLine();
        if (ImGui::Button("New"))
            newEntity();

        ImGui::Separator();

        ImGui::BeginChild("##EntityList", ImVec2(220.0f, 0.0f), true);

        for (int i = 0; i < static_cast<int>(m_entries.size()); ++i)
        {
            const auto &e = m_entries[i];
            const bool selected = (i == m_selectedIndex);

            std::string label = e.name;
            if (e.isEditorOwned)
                label += "  (editor)";

            if (ImGui::Selectable(label.c_str(), selected))
            {
                m_selectedIndex = i;
                loadSelected();
            }
        }

        ImGui::EndChild();
    }

    void EntityTypeEditor::drawEntityDetails(Engine::ECS::ECSContext & /*ecs*/, Engine::AssetManager & /*assets*/)
    {
        // Name + model
        ImGui::InputText("Name", m_nameBuf, sizeof(m_nameBuf));
        ImGui::InputText("Visual Model Path", m_modelBuf, sizeof(m_modelBuf));

        ImGui::SameLine();
        if (ImGui::Button("Rescan Models"))
            m_smodelsDirty = true;

        if (m_smodelsDirty)
            refreshSModelList();

        const std::string currentModelPath = trim(std::string(m_modelBuf));
        const std::string currentModelFile = fileNameFromPath(currentModelPath);
        const char *preview = currentModelFile.empty() ? "(none)" : currentModelFile.c_str();

        if (ImGui::BeginCombo("Pick .smodel", preview))
        {
            for (const auto &m : m_smodels)
            {
                const bool selected = (!currentModelPath.empty() && m.runtimePath == currentModelPath);
                if (ImGui::Selectable(m.fileName.c_str(), selected))
                    std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", m.runtimePath.c_str());
                if (selected)
                    ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }

        if (!m_loadedFromPath.empty())
        {
            ImGui::TextDisabled("Loaded from: %s", m_loadedFromPath.c_str());
        }

        ImGui::Separator();

        // Components + defaults
        auto &defs = defaultsObj();

        struct CompRow
        {
            const char *name;
            bool hasDefaults;
        };

        static const CompRow kKnown[] = {
            {"Position", true},
            {"Velocity", true},
            {"Health", true},
            {"MoveTarget", true},
            {"MoveSpeed", true},
            {"Radius", true},
            {"Separation", true},
            {"AvoidanceParams", true},
            {"Facing", true},
            {"Path", true},
            {"Team", true},
            {"AttackCooldown", true},
            {"LocomotionClips", true},
            {"CombatClips", true},
            {"Obstacle", true},
            {"ObstacleRadius", true},
            {"RenderMesh", false},
        };

        if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
        {
            for (const auto &c : kKnown)
            {
                bool enabled = hasComponent(c.name);
                if (ImGui::Checkbox(c.name, &enabled))
                {
                    ensureComponent(c.name, enabled);
                    if (enabled && c.hasDefaults)
                    {
                        ensureDefaultsObject(m_doc);
                        if (!defs.contains(c.name))
                            defs[c.name] = nlohmann::json::object();
                    }
                }
            }

            ImGui::Spacing();
            ImGui::InputText("Custom Component", m_customCompBuf, sizeof(m_customCompBuf));
            ImGui::SameLine();
            if (ImGui::Button("Add##CustomComp"))
            {
                const std::string cc = trim(std::string(m_customCompBuf));
                if (!cc.empty())
                {
                    ensureComponent(cc, true);
                    m_customCompBuf[0] = '\0';
                }
            }
        }

        if (ImGui::CollapsingHeader("Defaults", ImGuiTreeNodeFlags_DefaultOpen))
        {
            auto drawVec3 = [&](const char *comp, const char *xk, const char *yk, const char *zk, float def)
            {
                if (!hasComponent(comp))
                    return;
                ensureDefaultsObject(m_doc);
                auto &o = defs[comp];
                if (!o.is_object())
                    o = nlohmann::json::object();

                float x = getFloatOr(o, xk, def);
                float y = getFloatOr(o, yk, def);
                float z = getFloatOr(o, zk, def);

                if (ImGui::TreeNode(comp))
                {
                    if (ImGui::DragFloat("x", &x, 0.1f))
                        o[xk] = x;
                    if (ImGui::DragFloat("y", &y, 0.1f))
                        o[yk] = y;
                    if (ImGui::DragFloat("z", &z, 0.1f))
                        o[zk] = z;
                    ImGui::TreePop();
                }
            };

            auto drawFloat = [&](const char *comp, const char *k, float def, float speed = 0.1f)
            {
                if (!hasComponent(comp))
                    return;
                ensureDefaultsObject(m_doc);
                auto &o = defs[comp];
                if (!o.is_object())
                    o = nlohmann::json::object();

                float v = getFloatOr(o, k, def);
                if (ImGui::TreeNode(comp))
                {
                    if (ImGui::DragFloat(k, &v, speed))
                        o[k] = v;
                    ImGui::TreePop();
                }
            };

            // Position/Velocity
            drawVec3("Position", "x", "y", "z", 0.0f);
            drawVec3("Velocity", "x", "y", "z", 0.0f);

            // Health
            if (hasComponent("Health"))
            {
                auto &o = defs["Health"];
                float hp = getFloatOr(o, "value", 100.0f);
                if (ImGui::TreeNode("Health"))
                {
                    if (ImGui::DragFloat("value", &hp, 1.0f, 1.0f, 10000.0f))
                        o["value"] = hp;
                    ImGui::TreePop();
                }
            }

            // MoveSpeed
            drawFloat("MoveSpeed", "value", 5.0f, 0.1f);

            // Radius
            if (hasComponent("Radius"))
            {
                auto &o = defs["Radius"];
                float r = getFloatOr(o, "r", 0.5f);
                if (ImGui::TreeNode("Radius"))
                {
                    if (ImGui::DragFloat("r", &r, 0.05f, 0.0f, 50.0f))
                        o["r"] = r;
                    ImGui::TreePop();
                }
            }

            // Separation
            drawFloat("Separation", "value", 0.5f, 0.05f);

            // Facing
            if (hasComponent("Facing"))
            {
                auto &o = defs["Facing"];
                float yaw = getFloatOr(o, "yaw", 0.0f);
                if (ImGui::TreeNode("Facing"))
                {
                    if (ImGui::DragFloat("yaw", &yaw, 0.01f))
                        o["yaw"] = yaw;
                    ImGui::TreePop();
                }
            }

            // Team
            if (hasComponent("Team"))
            {
                auto &o = defs["Team"];
                int id = getIntOr(o, "id", 0);
                if (ImGui::TreeNode("Team"))
                {
                    if (ImGui::InputInt("id", &id))
                    {
                        if (id < 0)
                            id = 0;
                        if (id > 255)
                            id = 255;
                        o["id"] = id;
                    }
                    ImGui::TreePop();
                }
            }

            // MoveTarget
            if (hasComponent("MoveTarget"))
            {
                auto &o = defs["MoveTarget"];
                float x = getFloatOr(o, "x", 0.0f);
                float y = getFloatOr(o, "y", 0.0f);
                float z = getFloatOr(o, "z", 0.0f);
                int active = getIntOr(o, "active", 0);

                if (ImGui::TreeNode("MoveTarget"))
                {
                    if (ImGui::DragFloat("x", &x, 0.1f))
                        o["x"] = x;
                    if (ImGui::DragFloat("y", &y, 0.1f))
                        o["y"] = y;
                    if (ImGui::DragFloat("z", &z, 0.1f))
                        o["z"] = z;
                    if (ImGui::Checkbox("active", reinterpret_cast<bool *>(&active)))
                        o["active"] = active ? 1 : 0;
                    ImGui::TreePop();
                }
            }

            // AttackCooldown
            if (hasComponent("AttackCooldown"))
            {
                auto &o = defs["AttackCooldown"];
                float timer = getFloatOr(o, "timer", 0.0f);
                float interval = getFloatOr(o, "interval", 1.0f);
                if (ImGui::TreeNode("AttackCooldown"))
                {
                    if (ImGui::DragFloat("timer", &timer, 0.05f, 0.0f, 999.0f))
                        o["timer"] = timer;
                    if (ImGui::DragFloat("interval", &interval, 0.05f, 0.01f, 60.0f))
                        o["interval"] = interval;
                    ImGui::TreePop();
                }
            }

            // AvoidanceParams
            if (hasComponent("AvoidanceParams"))
            {
                auto &o = defs["AvoidanceParams"];
                if (!o.is_object())
                    o = nlohmann::json::object();

                auto drag = [&](const char *k, float def, float speed = 0.05f)
                {
                    float v = getFloatOr(o, k, def);
                    if (ImGui::DragFloat(k, &v, speed))
                        o[k] = v;
                };

                if (ImGui::TreeNode("AvoidanceParams"))
                {
                    drag("strength", 3.0f);
                    drag("maxAccel", 30.0f);
                    drag("blend", 0.6f);
                    drag("predictionTime", 0.55f);
                    drag("interactSlack", 0.35f);
                    drag("falloffWeight", 0.35f);
                    drag("predictiveWeight", 0.75f);
                    drag("pressureBoost", 1.5f);
                    drag("nearGoalRadius", 2.0f);
                    drag("nearGoalBoost", 2.5f);
                    drag("stoppedBoost", 2.0f);
                    drag("maxStopSpeed", 0.9f);
                    ImGui::TreePop();
                }
            }

            // LocomotionClips
            if (hasComponent("LocomotionClips"))
            {
                auto &o = defs["LocomotionClips"];
                int idle = getIntOr(o, "idleClip", 0);
                int walk = getIntOr(o, "walkClip", 0);
                int run = getIntOr(o, "runClip", 0);

                if (ImGui::TreeNode("LocomotionClips"))
                {
                    if (ImGui::InputInt("idleClip", &idle))
                        o["idleClip"] = idle;
                    if (ImGui::InputInt("walkClip", &walk))
                        o["walkClip"] = walk;
                    if (ImGui::InputInt("runClip", &run))
                        o["runClip"] = run;
                    ImGui::TreePop();
                }
            }

            // CombatClips
            if (hasComponent("CombatClips"))
            {
                auto &o = defs["CombatClips"];
                auto get = [&](const char *k)
                { return getIntOr(o, k, 0); };
                int attackStart = get("attackStart");
                int attackEnd = get("attackEnd");
                int damageStart = get("damageStart");
                int damageEnd = get("damageEnd");
                int deathStart = get("deathStart");
                int deathEnd = get("deathEnd");

                if (ImGui::TreeNode("CombatClips"))
                {
                    if (ImGui::InputInt("attackStart", &attackStart))
                        o["attackStart"] = attackStart;
                    if (ImGui::InputInt("attackEnd", &attackEnd))
                        o["attackEnd"] = attackEnd;
                    if (ImGui::InputInt("damageStart", &damageStart))
                        o["damageStart"] = damageStart;
                    if (ImGui::InputInt("damageEnd", &damageEnd))
                        o["damageEnd"] = damageEnd;
                    if (ImGui::InputInt("deathStart", &deathStart))
                        o["deathStart"] = deathStart;
                    if (ImGui::InputInt("deathEnd", &deathEnd))
                        o["deathEnd"] = deathEnd;
                    ImGui::TreePop();
                }
            }

            // ObstacleRadius
            if (hasComponent("ObstacleRadius"))
            {
                auto &o = defs["ObstacleRadius"];
                float r = getFloatOr(o, "r", 1.0f);
                if (ImGui::TreeNode("ObstacleRadius"))
                {
                    if (ImGui::DragFloat("r", &r, 0.05f, 0.0f, 100.0f))
                        o["r"] = r;
                    ImGui::TreePop();
                }
            }

            // Path/Obstacle are empty objects
            if (hasComponent("Path"))
                defs["Path"] = nlohmann::json::object();
            if (hasComponent("Obstacle"))
                defs["Obstacle"] = nlohmann::json::object();
        }

        ImGui::Separator();

        std::string error;

        if (ImGui::Button("Save (Editor-owned)"))
        {
            if (saveToEditorEntities(&error))
            {
                m_statusMsg = "Saved (editor-owned)";
                m_statusTimer = 2.0f;
            }
            else
            {
                m_statusMsg = error;
                m_statusTimer = 3.0f;
            }
        }

        ImGui::SameLine();

        if (m_loadedFromEditor)
        {
            if (ImGui::Button("Delete Editor Override"))
            {
                if (!deleteEditorEntity(&error))
                {
                    m_statusMsg = error;
                    m_statusTimer = 3.0f;
                }
            }
        }

        if (m_statusTimer > 0.0f && !m_statusMsg.empty())
        {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", m_statusMsg.c_str());
        }
    }

    void EntityTypeEditor::draw(Engine::ECS::ECSContext &ecs, Engine::AssetManager &assets, const Callbacks &callbacks)
    {
        if (m_listDirty)
            refreshList();

        if (m_statusTimer > 0.0f)
        {
            m_statusTimer -= ImGui::GetIO().DeltaTime;
            if (m_statusTimer < 0.0f)
                m_statusTimer = 0.0f;
        }

        if (!ImGui::CollapsingHeader("Entity Types", ImGuiTreeNodeFlags_DefaultOpen))
            return;

        // Two-column layout: list on the left, details on the right
        ImGui::Columns(2, "##EntityColumns", true);
        drawEntityList();
        ImGui::NextColumn();

        drawEntityDetails(ecs, assets);

        ImGui::Columns(1);

        // If we changed files, allow quick world refresh
        if (ImGui::Button("Reload Prefabs + Respawn World"))
        {
            if (callbacks.reloadPrefabsAndRespawn)
                callbacks.reloadPrefabsAndRespawn(callbacks.user);
        }
    }

}
