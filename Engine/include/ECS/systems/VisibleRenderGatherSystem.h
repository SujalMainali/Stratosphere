#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/VisibleRender.h"
#include "utils/JobSystem.h"

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <algorithm>
#include <iostream>
#endif

#include <unordered_map>

class VisibleRenderGatherSystem : public Engine::ECS::SystemBase
{
public:
    // =====================
    // TUNING CONSTANTS
    // =====================
    // Parallelize when total renderables across matching stores exceeds this threshold.
    static constexpr uint32_t PARALLEL_TOTAL_ROW_THRESHOLD = 40;

    // Chunk size (rows per task) for parallel gather.
    static constexpr uint32_t PARALLEL_CHUNK_SIZE = 40;

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

        // Build chunk list and total row count.
        m_chunks.clear();
        uint32_t totalRows = 0;
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

            const uint32_t n = store.size();
            if (n == 0u)
                continue;

            totalRows += n;

            if (PARALLEL_CHUNK_SIZE == 0u)
            {
                Chunk c;
                c.archetypeId = archetypeId;
                c.startRow = 0u;
                c.endRow = n;
                m_chunks.push_back(c);
            }
            else
            {
                for (uint32_t start = 0u; start < n; start += PARALLEL_CHUNK_SIZE)
                {
                    Chunk c;
                    c.archetypeId = archetypeId;
                    c.startRow = start;
                    c.endRow = std::min<uint32_t>(n, start + PARALLEL_CHUNK_SIZE);
                    m_chunks.push_back(c);
                }
            }
        }

        const bool canParallel = (ecs.jobSystem && totalRows >= PARALLEL_TOTAL_ROW_THRESHOLD && !m_chunks.empty());

        if (canParallel)
        {
            const uint32_t scratchCount = ecs.jobSystem ? (ecs.jobSystem->workerCount() + 1u) : 1u;
            if (m_workerScratch.size() < scratchCount)
                m_workerScratch.resize(scratchCount);
            for (uint32_t i = 0u; i < scratchCount; ++i)
                m_workerScratch[i].clearFrame();

            ecs.jobSystem->parallelFor(static_cast<uint32_t>(m_chunks.size()), [&](uint32_t workerIndex, uint32_t chunkIndex)
                                       {
                                           const uint32_t scratchIdx = std::min<uint32_t>(workerIndex, scratchCount - 1u);
                                           WorkerScratch &scratch = m_workerScratch[scratchIdx];

                                           const Chunk &chunk = m_chunks[chunkIndex];
                                           auto *ptr = ecs.stores.get(chunk.archetypeId);
                                           if (!ptr)
                                               return;
                                           auto &store = *ptr;
                                           if (!store.hasRenderModel() || !store.hasRenderTransform() || !store.hasPosePalette() || !store.hasVisibilityState())
                                               return;

                                           const auto &entities = store.entities();
                                           const auto &renderModels = store.renderModels();
                                           const auto &renderTransforms = store.renderTransforms();
                                           const auto &posePalettes = store.posePalettes();
                                           const auto &visibilityStates = store.visibilityState();

                                           const uint32_t n = store.size();
                                           const uint32_t start = std::min(chunk.startRow, n);
                                           const uint32_t end = std::min(chunk.endRow, n);

                                           for (uint32_t row = start; row < end; ++row)
                                           {
                                               if (!visibilityStates[row].visible)
                                                   continue;

                                               const Engine::ModelHandle handle = renderModels[row].handle;
                                               const uint64_t key = keyFromHandle(handle);

                                               auto &bucket = scratch.byModel[key];
                                               if (bucket.refs.empty())
                                               {
                                                   bucket.handle = handle;
                                                   scratch.activeModelKeys.push_back(key);
                                               }

                                               Engine::ECS::VisibleRenderRef ref{};
                                               ref.entity = entities[row];
                                               ref.archetypeId = chunk.archetypeId;
                                               ref.row = row;
                                               ref.model = handle;
                                               ref.modelKey = key;
                                               ref.transformVersion = renderTransforms[row].transformVersion;
                                               ref.poseVersion = posePalettes[row].poseVersion;
                                               ref.visibleFrame = visibilityStates[row].visibleFrame;
                                               ref.justBecameVisible = !visibilityStates[row].wasVisibleLastFrame;

                                               bucket.refs.emplace_back(ref);
                                               scratch.visibleRenderables += 1u;
                                           }
                                       });

            // Serial merge into shared buckets.
            m_buckets.totalRenderables = totalRows;
            m_buckets.visibleRenderables = 0u;
            m_buckets.activeModelKeys.clear();

            for (uint32_t i = 0u; i < scratchCount; ++i)
            {
                WorkerScratch &scratch = m_workerScratch[i];
                m_buckets.visibleRenderables += scratch.visibleRenderables;

                for (uint64_t key : scratch.activeModelKeys)
                {
                    auto it = scratch.byModel.find(key);
                    if (it == scratch.byModel.end())
                        continue;

                    WorkerBucket &localBucket = it->second;

                    auto &bucket = m_buckets.byModel[key];
                    if (bucket.lastUsedFrame != m_frameCounter)
                    {
                        bucket.lastUsedFrame = m_frameCounter;
                        bucket.handle = localBucket.handle;
                        bucket.refs.clear();
                        m_buckets.activeModelKeys.push_back(key);
                    }

                    bucket.refs.insert(bucket.refs.end(),
                                      std::make_move_iterator(localBucket.refs.begin()),
                                      std::make_move_iterator(localBucket.refs.end()));
                }
            }
        }
        else
        {
            // Sequential fallback (existing behavior)
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
                    ref.visibleFrame = visibilityStates[row].visibleFrame;
                    ref.justBecameVisible = !visibilityStates[row].wasVisibleLastFrame;

                    bucket.refs.emplace_back(ref);
                    m_buckets.visibleRenderables += 1u;
                }
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
    struct Chunk
    {
        uint32_t archetypeId = UINT32_MAX;
        uint32_t startRow = 0u;
        uint32_t endRow = 0u;
    };

    struct WorkerBucket
    {
        Engine::ModelHandle handle{};
        std::vector<Engine::ECS::VisibleRenderRef> refs;
    };

    struct WorkerScratch
    {
        uint32_t visibleRenderables = 0u;
        std::unordered_map<uint64_t, WorkerBucket> byModel;
        std::vector<uint64_t> activeModelKeys;

        void clearFrame()
        {
            visibleRenderables = 0u;
            activeModelKeys.clear();
            byModel.clear();
        }
    };

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_frameCounter = 0;

    Engine::ECS::VisibleRenderBuckets m_buckets;

    std::vector<Chunk> m_chunks;
    std::vector<WorkerScratch> m_workerScratch;
};
