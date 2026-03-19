#pragma once

#include "ECS/SystemFormat.h"

#include "assets/AssetManager.h"

#include "Engine/Camera.h"
#include "Engine/Renderer.h"
#include "Engine/SModelRenderPassModule.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <iostream>
#endif

class RenderSystem : public Engine::ECS::SystemBase
{
public:
    explicit RenderSystem(Engine::AssetManager *assets = nullptr)
        : m_assets(assets)
    {
        // RenderModel provides the model handle.
        // PosePalette provides per-entity node/joint palettes.
        // RenderTransform provides the cached world matrix.
        // VisibilityState provides visibility information for culling.
        setRequiredNames({"RenderModel", "PosePalette", "RenderTransform", "VisibilityState"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderModelSystem"; }
    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void setRenderer(Engine::Renderer *renderer) { m_renderer = renderer; }
    void setCamera(Engine::Camera *camera) { m_camera = camera; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;
        if (!m_assets || !m_renderer || !m_camera)
            return;

        Stats frameStats{};

        auto keyFromHandle = [](const Engine::ModelHandle &h) -> uint64_t
        {
            return (static_cast<uint64_t>(h.generation) << 32) | static_cast<uint64_t>(h.id);
        };

        auto fnv1aMixU32 = [](uint64_t h, uint32_t v) -> uint64_t
        {
            // FNV-1a 64-bit (mix 4 bytes in a stable way)
            h ^= static_cast<uint64_t>(v) & 0xFFu;
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 8) & 0xFFu);
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 16) & 0xFFu);
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 24) & 0xFFu);
            h *= 1099511628211ull;
            return h;
        };

        m_frameCounter += 1u;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *ptr = ecs.stores.get(archetypeId);
            if (!ptr)
                continue;

            auto &store = *ptr;
            if (!store.signature().containsAll(required()))
                continue;
            if (!store.signature().containsNone(excluded()))
                continue;
            if (!store.hasRenderModel())
                continue;
            if (!store.hasPosePalette())
                continue;
            if (!store.hasRenderTransform())
                continue;
            if (!store.hasVisibilityState())
                continue;

            auto &renderModels = store.renderModels();
            auto &renderTransforms = store.renderTransforms();
            auto &posePalettes = store.posePalettes();
            auto &visibilityStates = store.visibilityState();
            auto &entities = store.entities();
            const uint32_t n = store.size();
            frameStats.totalCandidates += n;

            for (uint32_t row = 0; row < n; ++row)
            {
                // Skip invisible entities
                if (!visibilityStates[row].visible)
                {
                    frameStats.invisibleSkipped += 1u;
                    continue;
                }

                frameStats.visibleProcessed += 1u;

                const Engine::ModelHandle handle = renderModels[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                if (!asset)
                    continue;

                const uint64_t key = keyFromHandle(handle);

                auto &batch = m_batchesByModel[key];
                if (batch.lastUsedFrame != m_frameCounter)
                {
                    batch.lastUsedFrame = m_frameCounter;
                    batch.handle = handle;

                    batch.contentHash = 1469598103934665603ull;
                    batch.contentHash = fnv1aMixU32(batch.contentHash, static_cast<uint32_t>(handle.id));
                    batch.contentHash = fnv1aMixU32(batch.contentHash, static_cast<uint32_t>(handle.generation));

                    batch.instanceWorlds.clear();
                    batch.nodePalette.clear();
                    batch.jointPalette.clear();

                    // Prefer counts from PosePalette (it is what we will upload).
                    batch.nodeCount = posePalettes[row].nodeCount;
                    batch.jointCount = posePalettes[row].jointCount;
                    if (batch.nodeCount == 0)
                        batch.nodeCount = static_cast<uint32_t>(asset->nodes.size());
                    if (batch.jointCount == 0)
                        batch.jointCount = asset->totalJointCount;
                }

                if (batch.nodeCount == 0)
                    continue;

                // World matrix comes from RenderTransform (updated by RenderTransformUpdateSystem).
                const glm::mat4 &world = renderTransforms[row].world;

                // Hash the semantic inputs that should affect rendering.
                // This lets us skip re-uploading identical instance/palette data to the GPU.
                const auto &e = entities[row];
                batch.contentHash = fnv1aMixU32(batch.contentHash, e.index);
                batch.contentHash = fnv1aMixU32(batch.contentHash, e.generation);
                batch.contentHash = fnv1aMixU32(batch.contentHash, renderTransforms[row].transformVersion);
                batch.contentHash = fnv1aMixU32(batch.contentHash, posePalettes[row].poseVersion);

                batch.instanceWorlds.emplace_back(world);
                frameStats.submittedInstances += 1u;

                // Palettes come from PosePalette component.
                const auto &pose = posePalettes[row];
                if (pose.nodeCount == batch.nodeCount && pose.nodePalette.size() == static_cast<size_t>(batch.nodeCount))
                {
                    batch.nodePalette.insert(batch.nodePalette.end(), pose.nodePalette.begin(), pose.nodePalette.end());
                }
                else
                {
                    batch.nodePalette.insert(batch.nodePalette.end(), batch.nodeCount, glm::mat4(1.0f));
                }

                if (batch.jointCount > 0)
                {
                    if (pose.jointCount == batch.jointCount && pose.jointPalette.size() == static_cast<size_t>(batch.jointCount))
                        batch.jointPalette.insert(batch.jointPalette.end(), pose.jointPalette.begin(), pose.jointPalette.end());
                    else
                        batch.jointPalette.insert(batch.jointPalette.end(), batch.jointCount, glm::mat4(1.0f));
                }

                (void)asset;
            }
        }

        // Create/update passes for models that have instances this frame.
        for (auto &kv : m_batchesByModel)
        {
            const uint64_t key = kv.first;
            auto &batch = kv.second;
            if (batch.lastUsedFrame != m_frameCounter)
                continue;
            auto &worlds = batch.instanceWorlds;
            if (worlds.empty())
                continue;

            const Engine::ModelHandle handle = batch.handle;

            auto it = m_passes.find(key);
            if (it == m_passes.end())
            {
                PassEntry entry;
                entry.pass = std::make_shared<Engine::SModelRenderPassModule>();
                entry.pass->setAssets(m_assets);
                entry.pass->setModel(handle);
                entry.pass->setCamera(m_camera);
                entry.pass->setEnabled(true);
                m_renderer->registerPass(entry.pass);
                it = m_passes.emplace(key, std::move(entry)).first;
            }

            auto &entry = it->second;
            entry.pass->setCamera(m_camera);
            entry.pass->setEnabled(true);

            // If nothing changed for this model batch, keep the previous GPU buffers.
            if (entry.lastSubmittedHash != batch.contentHash)
            {
                entry.lastSubmittedHash = batch.contentHash;

                entry.pass->setInstances(worlds.data(), static_cast<uint32_t>(worlds.size()));
                entry.pass->setNodePalette(batch.nodePalette.data(), static_cast<uint32_t>(worlds.size()), batch.nodeCount);

                if (batch.jointCount > 0 && batch.jointPalette.size() == worlds.size() * static_cast<size_t>(batch.jointCount))
                {
                    entry.pass->setJointPalette(batch.jointPalette.data(), static_cast<uint32_t>(worlds.size()), batch.jointCount);
                }
            }
        }

        // Disable passes that have no instances this frame.
        for (auto &kv : m_passes)
        {
            const auto it = m_batchesByModel.find(kv.first);
            if (it == m_batchesByModel.end() || it->second.lastUsedFrame != m_frameCounter)
            {
                kv.second.pass->setEnabled(false);
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            std::cout << "[RenderSystem] frame=" << m_frameCounter
                      << " candidates=" << frameStats.totalCandidates
                      << " visible=" << frameStats.visibleProcessed
                      << " submitted=" << frameStats.submittedInstances
                      << " skippedInvisible=" << frameStats.invisibleSkipped
                      << "\n";
        }
#endif
    }

private:
    struct Stats
    {
        uint32_t totalCandidates = 0;
        uint32_t visibleProcessed = 0;
        uint32_t submittedInstances = 0;
        uint32_t invisibleSkipped = 0;
    };

    struct PerModelBatch
    {
        Engine::ModelHandle handle{};
        uint32_t lastUsedFrame = 0;

        uint64_t contentHash = 0;

        std::vector<glm::mat4> instanceWorlds;
        std::vector<glm::mat4> nodePalette; // flattened: [instance][node]
        uint32_t nodeCount = 0;

        std::vector<glm::mat4> jointPalette; // flattened: [instance][joint]
        uint32_t jointCount = 0;
    };

    struct PassEntry
    {
        std::shared_ptr<Engine::SModelRenderPassModule> pass;
        uint64_t lastSubmittedHash = 0;
    };

    Engine::AssetManager *m_assets = nullptr; // not owned
    Engine::Renderer *m_renderer = nullptr;   // not owned
    Engine::Camera *m_camera = nullptr;       // not owned

    std::unordered_map<uint64_t, PassEntry> m_passes;
    std::unordered_map<uint64_t, PerModelBatch> m_batchesByModel;
    uint32_t m_frameCounter = 0;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
};
