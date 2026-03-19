#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/VisibleRender.h"

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <algorithm>
#include <iostream>
#endif

class VisibleRenderGatherSystem : public Engine::ECS::SystemBase
{
public:
    VisibleRenderGatherSystem()
    {
        setRequiredNames({"RenderModel", "RenderTransform", "PosePalette", "VisibilityState"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "VisibleRenderGatherSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    const Engine::ECS::VisibleRenderBuckets &buckets() const { return m_buckets; }
    Engine::ECS::VisibleRenderBuckets &bucketsMut() { return m_buckets; }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        ++m_frameCounter;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        m_buckets.frame = m_frameCounter;
        m_buckets.totalRenderables = 0;
        m_buckets.visibleRenderables = 0;
        m_buckets.activeModelKeys.clear();

        auto keyFromHandle = [](const Engine::ModelHandle &h) -> uint64_t
        {
            return (static_cast<uint64_t>(h.generation) << 32) | static_cast<uint64_t>(h.id);
        };

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
            if (!store.hasRenderModel() || !store.hasRenderTransform() || !store.hasPosePalette() || !store.hasVisibilityState())
                continue;

            const auto &entities = store.entities();
            const auto &renderModels = store.renderModels();
            const auto &renderTransforms = store.renderTransforms();
            const auto &posePalettes = store.posePalettes();
            const auto &visibilityStates = store.visibilityState();
            const uint32_t n = store.size();

            m_buckets.totalRenderables += n;

            for (uint32_t row = 0; row < n; ++row)
            {
                if (!visibilityStates[row].visible)
                    continue;

                const Engine::ModelHandle handle = renderModels[row].handle;
                const uint64_t key = keyFromHandle(handle);

                auto &bucket = m_buckets.byModel[key];
                if (bucket.lastUsedFrame != m_frameCounter)
                {
                    bucket.lastUsedFrame = m_frameCounter;
                    bucket.handle = handle;
                    bucket.refs.clear();
                    m_buckets.activeModelKeys.push_back(key);
                }

                Engine::ECS::VisibleRenderRef ref{};
                ref.entity = entities[row];
                ref.archetypeId = archetypeId;
                ref.row = row;
                ref.model = handle;
                ref.modelKey = key;
                ref.transformVersion = renderTransforms[row].transformVersion;
                ref.poseVersion = posePalettes[row].poseVersion;

                bucket.refs.emplace_back(ref);
                m_buckets.visibleRenderables += 1u;
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            std::cout << "[VisibleRenderGatherSystem] frame=" << m_frameCounter
                      << " totalRenderables=" << m_buckets.totalRenderables
                      << " visibleRenderables=" << m_buckets.visibleRenderables
                      << " visibleModels=" << static_cast<uint32_t>(m_buckets.activeModelKeys.size())
                      << "\n";

            // Print a small "visible per model" summary (top-N by instance count).
            std::vector<std::pair<uint32_t, uint64_t>> counts;
            counts.reserve(m_buckets.activeModelKeys.size());
            for (uint64_t key : m_buckets.activeModelKeys)
            {
                auto it = m_buckets.byModel.find(key);
                if (it == m_buckets.byModel.end())
                    continue;
                counts.emplace_back(static_cast<uint32_t>(it->second.refs.size()), key);
            }

            std::sort(counts.begin(), counts.end(), [](const auto &a, const auto &b)
                      { return a.first > b.first; });

            const uint32_t topN = std::min<uint32_t>(static_cast<uint32_t>(counts.size()), 5u);
            if (topN > 0u)
            {
                std::cout << "[VisibleRenderGatherSystem] topVisibleModels=";
                for (uint32_t i = 0; i < topN; ++i)
                {
                    auto it = m_buckets.byModel.find(counts[i].second);
                    if (it == m_buckets.byModel.end())
                        continue;
                    const auto &h = it->second.handle;
                    std::cout << " (" << h.id << ":" << h.generation << "," << counts[i].first << ")";
                }
                std::cout << "\n";
            }
        }
#endif
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_frameCounter = 0;

    Engine::ECS::VisibleRenderBuckets m_buckets;
};
