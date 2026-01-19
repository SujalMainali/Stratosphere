#pragma once

#include "ECS/SystemFormat.h"

#include "assets/AssetManager.h"
#include "assets/MeshAsset.h"

class RenderSystem : public Engine::ECS::SystemBase
{
public:
    explicit RenderSystem(Engine::AssetManager *assets = nullptr)
        : m_assets(assets)
    {
        // We need Position to build a model matrix later.
        setRequiredNames({"RenderMesh", "Position"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderMeshSystem"; }

    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void update(Engine::ECS::ArchetypeStoreManager &mgr, float /*dt*/) override
    {
        if (!m_assets)
            return;

        for (const auto &ptr : mgr.stores())
        {
            if (!ptr)
                continue;

            auto &store = *ptr;
            if (!store.signature().containsAll(required()))
                continue;
            if (!store.signature().containsNone(excluded()))
                continue;
            if (!store.hasRenderModel())
                continue;
            if (!store.hasPosition())
                continue;

            auto &renderModels = const_cast<std::vector<Engine::ECS::RenderModel> &>(store.renderModels());
            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            const auto &masks = store.rowMasks();
            const uint32_t n = store.size();

            for (uint32_t row = 0; row < n; ++row)
            {
                if (!masks[row].matches(required(), excluded()))
                    continue;

                const Engine::ModelHandle handle = renderModels[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                if (!asset)
                    continue;

                const auto &pos = positions[row];

                // TODO use the asset and the positon to properly display the entity.
                (void)asset;
                (void)pos;
            }
        }
    }

private:
    Engine::AssetManager *m_assets = nullptr; // not owned
};
