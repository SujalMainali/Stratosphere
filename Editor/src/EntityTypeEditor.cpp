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
    using EntityKind = Editor::EntityTypeEditor::EntityKind;

    static const char *kCombatantComponents[] = {
        "Position",
        "Velocity",
        "Health",
        "MoveTarget",
        "MoveSpeed",
        "Radius",
        "Separation",
        "AvoidanceParams",
        "Facing",
        "Path",
        "Team",
        "AttackCooldown",
        "LocomotionClips",
        "CombatClips",
    };

    static const char *kObstacleComponents[] = {
        "Position",
        "Obstacle",
        "ObstacleRadius",
    };

    static bool isEditorVisibleComponentName(const std::string &name)
    {
        // Hide runtime/internal components that are engine-maintained caches or editor-only tags.
        // The editor exposes visuals via the "Visual Model Path" field, so RenderModel/RenderAnimation
        // are also intentionally hidden here.
        static const std::vector<std::string> kHidden = {
            "Selected",
            "RenderModel",
            "RenderAnimation",
            "PosePalette",
            "RenderTransform",
            "RenderBounds",
            "VisibilityState",
        };

        if (name.empty())
            return false;

        return std::find(kHidden.begin(), kHidden.end(), name) == kHidden.end();
    }

    static const char *entityKindLabel(EntityKind kind)
    {
        switch (kind)
        {
        case EntityKind::None:
            return "None";
        case EntityKind::Obstacle:
            return "Obstacle";
        case EntityKind::Combatant:
        default:
            return "Combatant";
        }
    }

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

    static int InputTextCallback_Resize(ImGuiInputTextCallbackData *data)
    {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize)
        {
            auto *str = static_cast<std::string *>(data->UserData);
            str->resize(static_cast<size_t>(data->BufTextLen));
            data->Buf = str->data();
        }
        return 0;
    }

    static bool InputTextMultilineStdString(const char *label, std::string *str, const ImVec2 &size, ImGuiInputTextFlags flags = 0)
    {
        flags |= ImGuiInputTextFlags_CallbackResize;
        return ImGui::InputTextMultiline(label, str->data(), str->capacity() + 1, size, flags, InputTextCallback_Resize, str);
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
        m_entityKindIndex = static_cast<int>(detectEntityKindFromDoc());

        m_defaultsJsonBuf.clear();
        m_defaultsJsonErr.clear();

        m_statusMsg = "Loaded " + e.name;
        m_statusTimer = 2.0f;
    }

    void EntityTypeEditor::newEntity()
    {
        newEntity(EntityKind::Combatant);
    }

    void EntityTypeEditor::applyEntityKindDefaults(EntityKind kind)
    {
        if (kind == EntityKind::None)
            return;

        if (kind == EntityKind::Obstacle)
        {
            for (const char *c : kObstacleComponents)
                ensureComponent(c, true);
            return;
        }

        // Combatant defaults should be pre-selected on create, but still editable in Components UI.
        for (const char *c : kCombatantComponents)
            ensureComponent(c, true);
    }

    void EntityTypeEditor::applyEntityKindPreset(EntityKind kind)
    {
        // Re-apply only known preset components; custom components remain untouched.
        for (const char *c : kCombatantComponents)
            ensureComponent(c, false);
        for (const char *c : kObstacleComponents)
            ensureComponent(c, false);

        applyEntityKindDefaults(kind);
    }

    EntityKind EntityTypeEditor::detectEntityKindFromDoc() const
    {
        if (hasComponent("Obstacle") || hasComponent("ObstacleRadius"))
            return EntityKind::Obstacle;

        for (const char *c : kCombatantComponents)
        {
            if (hasComponent(c))
                return EntityKind::Combatant;
        }

        return EntityKind::None;
    }

    void EntityTypeEditor::newEntity(EntityKind kind)
    {
        m_doc = nlohmann::json::object();
        const char *defaultName = "NewCombatant";
        if (kind == EntityKind::Obstacle)
            defaultName = "NewObstacle";
        else if (kind == EntityKind::None)
            defaultName = "NewEntity";
        m_doc["name"] = defaultName;
        m_doc["components"] = nlohmann::json::array();
        m_doc["defaults"] = nlohmann::json::object();
        applyEntityKindDefaults(kind);

        std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", defaultName);
        m_modelBuf[0] = '\0';
        m_customCompBuf[0] = '\0';

        m_loadedFromPath.clear();
        m_loadedFromEditor = false;
        m_entityKindIndex = static_cast<int>(kind);

        m_defaultsJsonBuf.clear();
        m_defaultsJsonErr.clear();
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
            if (!m_doc.contains("visual") || !m_doc["visual"].is_object())
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
        {
            std::snprintf(m_createNameBuf, sizeof(m_createNameBuf), "%s", "NewCombatant");
            m_createKindIndex = static_cast<int>(EntityKind::Combatant);
            m_openCreatePopup = true;
        }

        if (m_openCreatePopup)
        {
            ImGui::OpenPopup("Create Entity Type");
            m_openCreatePopup = false;
        }

        if (ImGui::BeginPopupModal("Create Entity Type", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            static const char *kKinds[] = {"Combatant", "Obstacle", "None"};
            const int prevKind = m_createKindIndex;

            ImGui::InputText("Name", m_createNameBuf, sizeof(m_createNameBuf));
            ImGui::Combo("Entity Type", &m_createKindIndex, kKinds, IM_ARRAYSIZE(kKinds));

            if (m_createKindIndex != prevKind)
            {
                const char *suggested = "NewCombatant";
                if (m_createKindIndex == static_cast<int>(EntityKind::Obstacle))
                    suggested = "NewObstacle";
                else if (m_createKindIndex == static_cast<int>(EntityKind::None))
                    suggested = "NewEntity";
                std::snprintf(m_createNameBuf, sizeof(m_createNameBuf), "%s", suggested);
            }

            ImGui::Separator();
            if (ImGui::Button("Create"))
            {
                const EntityKind kind = (m_createKindIndex == static_cast<int>(EntityKind::Obstacle))
                                            ? EntityKind::Obstacle
                                            : ((m_createKindIndex == static_cast<int>(EntityKind::None)) ? EntityKind::None : EntityKind::Combatant);

                newEntity(kind);

                const std::string requestedName = trim(std::string(m_createNameBuf));
                if (!requestedName.empty())
                {
                    m_doc["name"] = requestedName;
                    std::snprintf(m_nameBuf, sizeof(m_nameBuf), "%s", requestedName.c_str());
                }

                m_statusMsg = std::string("Created ") + entityKindLabel(kind) + " entity template";
                m_statusTimer = 2.0f;
                ImGui::CloseCurrentPopup();
            }

            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                ImGui::CloseCurrentPopup();

            ImGui::EndPopup();
        }

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

    void EntityTypeEditor::drawEntityDetails(Engine::ECS::ECSContext &ecs, Engine::AssetManager & /*assets*/)
    {
        // Name + model
        ImGui::InputText("Name", m_nameBuf, sizeof(m_nameBuf));
        ImGui::InputText("Visual Model Path", m_modelBuf, sizeof(m_modelBuf));

        static const char *kKinds[] = {"Combatant", "Obstacle", "None"};
        const int prevKind = m_entityKindIndex;
        ImGui::Combo("Entity Type", &m_entityKindIndex, kKinds, IM_ARRAYSIZE(kKinds));
        if (m_entityKindIndex != prevKind)
        {
            const EntityKind kind = (m_entityKindIndex == static_cast<int>(EntityKind::Obstacle))
                                        ? EntityKind::Obstacle
                                        : ((m_entityKindIndex == static_cast<int>(EntityKind::None)) ? EntityKind::None : EntityKind::Combatant);
            applyEntityKindPreset(kind);
        }

        ImGui::SameLine();
        if (ImGui::Button("Rescan Models"))
            m_smodelsDirty = true;

        if (m_smodelsDirty)
            refreshSModelList();

        const std::string currentModelPath = trim(std::string(m_modelBuf));
        const char *preview = currentModelPath.empty() ? "(none)" : currentModelPath.c_str();

        if (ImGui::BeginCombo("Pick .smodel", preview))
        {
            for (const auto &m : m_smodels)
            {
                const bool selected = (!currentModelPath.empty() && m.runtimePath == currentModelPath);
                if (ImGui::Selectable(m.runtimePath.c_str(), selected))
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

        if (ImGui::CollapsingHeader("Components", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Ensure any components already present in the JSON are registered so they can appear.
            if (m_doc.contains("components") && m_doc["components"].is_array())
            {
                for (const auto &v : m_doc["components"])
                {
                    if (v.is_string())
                        (void)ecs.components.ensureId(v.get<std::string>());
                }
            }

            std::vector<std::string> all;
            all.reserve(static_cast<size_t>(ecs.components.count()) + 16);

            for (uint32_t cid = 0; cid < ecs.components.count(); ++cid)
            {
                const std::string &nm = ecs.components.getName(cid);
                if (!nm.empty())
                    all.push_back(nm);
            }

            // Also include any components referenced by defaults even if not in the registry yet.
            if (m_doc.contains("defaults") && m_doc["defaults"].is_object())
            {
                for (auto it = m_doc["defaults"].begin(); it != m_doc["defaults"].end(); ++it)
                {
                    if (!it.key().empty())
                        all.push_back(it.key());
                }
            }

            std::sort(all.begin(), all.end());
            all.erase(std::unique(all.begin(), all.end()), all.end());

            // Show enabled components first, then alphabetical.
            std::stable_sort(all.begin(), all.end(), [&](const std::string &a, const std::string &b)
                             {
                                 const bool ea = hasComponent(a);
                                 const bool eb = hasComponent(b);
                                 if (ea != eb)
                                     return ea > eb;
                                 return a < b;
                             });

            for (const auto &name : all)
            {
                if (!isEditorVisibleComponentName(name))
                    continue;

                bool enabled = hasComponent(name);
                if (ImGui::Checkbox(name.c_str(), &enabled))
                {
                    ensureComponent(name, enabled);
                    if (enabled)
                    {
                        // For components that support defaults, having an empty object is harmless and
                        // makes it easy for users to fill fields later.
                        ensureDefaultsObject(m_doc);
                        if (!defs.contains(name))
                            defs[name] = nlohmann::json::object();
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
                    (void)ecs.components.ensureId(cc);
                    m_customCompBuf[0] = '\0';
                }
            }
        }

        if (ImGui::CollapsingHeader("Defaults", ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Build the set of enabled components from the JSON doc.
            std::vector<std::string> enabled;
            if (m_doc.contains("components") && m_doc["components"].is_array())
            {
                for (const auto &v : m_doc["components"])
                {
                    if (!v.is_string())
                        continue;
                    std::string nm = v.get<std::string>();
                    if (!isEditorVisibleComponentName(nm))
                        continue;
                    enabled.push_back(std::move(nm));
                }
            }

            std::sort(enabled.begin(), enabled.end());
            enabled.erase(std::unique(enabled.begin(), enabled.end()), enabled.end());

            for (const auto &comp : enabled)
            {
                ensureDefaultsObject(m_doc);
                auto &o = defs[comp];
                if (!o.is_object())
                    o = nlohmann::json::object();

                if (!ImGui::TreeNode(comp.c_str()))
                    continue;

                bool handled = false;

                auto drawVec3Inline = [&](const char *xk, const char *yk, const char *zk, float def)
                {
                    float x = getFloatOr(o, xk, def);
                    float y = getFloatOr(o, yk, def);
                    float z = getFloatOr(o, zk, def);
                    if (ImGui::DragFloat("x", &x, 0.1f))
                        o[xk] = x;
                    if (ImGui::DragFloat("y", &y, 0.1f))
                        o[yk] = y;
                    if (ImGui::DragFloat("z", &z, 0.1f))
                        o[zk] = z;
                };

                if (comp == "Position" || comp == "Velocity")
                {
                    drawVec3Inline("x", "y", "z", 0.0f);
                    handled = true;
                }
                else if (comp == "Health")
                {
                    float hp = getFloatOr(o, "value", 100.0f);
                    if (ImGui::DragFloat("value", &hp, 1.0f, 1.0f, 10000.0f))
                        o["value"] = hp;
                    handled = true;
                }
                else if (comp == "MoveSpeed")
                {
                    float v = getFloatOr(o, "value", 5.0f);
                    if (ImGui::DragFloat("value", &v, 0.1f))
                        o["value"] = v;
                    handled = true;
                }
                else if (comp == "Radius" || comp == "ObstacleRadius")
                {
                    float r = getFloatOr(o, "r", 0.5f);
                    if (ImGui::DragFloat("r", &r, 0.05f, 0.0f, 50.0f))
                        o["r"] = r;
                    handled = true;
                }
                else if (comp == "Separation")
                {
                    float v = getFloatOr(o, "value", 0.5f);
                    if (ImGui::DragFloat("value", &v, 0.05f))
                        o["value"] = v;
                    handled = true;
                }
                else if (comp == "Facing")
                {
                    float yaw = getFloatOr(o, "yaw", 0.0f);
                    if (ImGui::DragFloat("yaw", &yaw, 0.01f))
                        o["yaw"] = yaw;
                    handled = true;
                }

                if (!handled)
                {
                    // Raw JSON fallback so defaults are editable for any component.
                    auto &buf = m_defaultsJsonBuf[comp];
                    if (buf.empty())
                        buf = o.dump(4);

                    ImGui::PushID(comp.c_str());
                    ImGui::TextDisabled("Defaults JSON (object)");
                    InputTextMultilineStdString("##DefaultsJson", &buf, ImVec2(-1.0f, 140.0f));

                    if (ImGui::Button("Apply##DefaultsJson"))
                    {
                        try
                        {
                            nlohmann::json parsed = nlohmann::json::parse(buf);
                            if (!parsed.is_object())
                            {
                                m_defaultsJsonErr[comp] = "Defaults must be a JSON object.";
                            }
                            else
                            {
                                o = std::move(parsed);
                                m_defaultsJsonErr.erase(comp);
                            }
                        }
                        catch (const std::exception &ex)
                        {
                            m_defaultsJsonErr[comp] = ex.what();
                        }
                    }

                    auto itErr = m_defaultsJsonErr.find(comp);
                    if (itErr != m_defaultsJsonErr.end() && !itErr->second.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", itErr->second.c_str());

                    ImGui::PopID();
                }

                ImGui::TreePop();
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
