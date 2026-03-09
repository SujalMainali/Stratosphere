#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace Engine::ECS
{
    struct ECSContext;
}

namespace Engine
{
    class AssetManager;
}

namespace Editor
{
    class EntityTypeEditor
    {
    public:
        struct Callbacks
        {
            // Called after save/delete when the world should be rebuilt.
            void (*reloadPrefabsAndRespawn)(void *user) = nullptr;
            void *user = nullptr;
        };

        explicit EntityTypeEditor(std::string editorEntitiesDir = "editor_entities");

        void draw(Engine::ECS::ECSContext &ecs, Engine::AssetManager &assets, const Callbacks &callbacks);

    private:
        struct SModelEntry
        {
            std::string fileName;    // display (just the .smodel name)
            std::string runtimePath; // stored in JSON (e.g. assets/Knight/Knight.smodel)
        };

        struct EntityEntry
        {
            std::string name;
            std::filesystem::path path;
            bool isEditorOwned = false; // true => editor_entities/, false => entities/
        };

        void refreshList();
        void loadSelected();

        void newEntity();

        void refreshSModelList();

        bool saveToEditorEntities(std::string *outError);
        bool deleteEditorEntity(std::string *outError);

        // UI helpers
        void drawEntityList();
        void drawEntityDetails(Engine::ECS::ECSContext &ecs, Engine::AssetManager &assets);

        // JSON helpers
        void ensureComponent(const std::string &comp, bool enabled);
        bool hasComponent(const std::string &comp) const;
        nlohmann::json &defaultsObj();
        const nlohmann::json &defaultsObj() const;

    private:
        std::string m_editorEntitiesDir;

        std::vector<SModelEntry> m_smodels;
        bool m_smodelsDirty = true;

        std::vector<EntityEntry> m_entries;
        int m_selectedIndex = -1;

        // Working JSON document
        nlohmann::json m_doc;

        // UI state
        char m_nameBuf[128] = {0};
        char m_modelBuf[256] = {0};
        char m_customCompBuf[64] = {0};

        std::string m_loadedFromPath;
        bool m_loadedFromEditor = false;

        std::string m_statusMsg;
        float m_statusTimer = 0.0f;

        bool m_listDirty = true;
    };
}
