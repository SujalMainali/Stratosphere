#pragma once

#include "ECS/SystemFormat.h"
#include "Engine/Frustum.h"
#include "Engine/Camera.h"
#include "utils/JobSystem.h"

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <iostream>
#endif

class VisibilityCullingSystem : public Engine::ECS::SystemBase
{
public:
    // =====================
    // TUNING CONSTANTS
    // =====================
    // Parallelize when total rows tested across all matching stores exceeds this threshold.
    static constexpr uint32_t PARALLEL_TOTAL_ROW_THRESHOLD = 40;

    // Chunk size (rows per task) for parallel culling.
    static constexpr uint32_t PARALLEL_CHUNK_SIZE = 40;

    VisibilityCullingSystem()
    {
        // Requires render bounds and visibility state
        setRequiredNames({"RenderBounds", "VisibilityState"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "VisibilityCullingSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_visibilityStateId = registry.ensureId("VisibilityState");
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void setCamera(Engine::Camera *camera) { m_camera = camera; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;

        if (!m_camera)
            return;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        m_frameCounter++;
        m_lastStats = Stats{};

        // Build frustum from camera
        glm::mat4 viewProj = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
        Engine::Frustum frustum = Engine::Frustum::fromViewProjection(viewProj);

        const auto &q = ecs.queries.get(m_queryId);

        // Build chunk list for parallel path (and compute total row count).
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
            if (!store.hasRenderBounds() || !store.hasVisibilityState())
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
            if (m_workerStats.size() < scratchCount)
                m_workerStats.resize(scratchCount);
            if (m_workerChanged.size() < scratchCount)
                m_workerChanged.resize(scratchCount);
            for (uint32_t i = 0u; i < scratchCount; ++i)
            {
                m_workerStats[i] = Stats{};
                m_workerChanged[i].clear();
            }

            ecs.jobSystem->parallelFor(static_cast<uint32_t>(m_chunks.size()), [&](uint32_t workerIndex, uint32_t chunkIndex)
                                       {
                                           const uint32_t scratchIdx = std::min<uint32_t>(workerIndex, scratchCount - 1u);
                                           Stats &stats = m_workerStats[scratchIdx];
                                           auto &changed = m_workerChanged[scratchIdx];

                                           const Chunk &chunk = m_chunks[chunkIndex];
                                           auto *ptr = ecs.stores.get(chunk.archetypeId);
                                           if (!ptr)
                                               return;
                                           auto &store = *ptr;
                                           if (!store.hasRenderBounds() || !store.hasVisibilityState())
                                               return;

                                           auto &renderBounds = store.renderBounds();
                                           auto &visibilityStates = store.visibilityState();
                                           const uint32_t n = store.size();
                                           const uint32_t start = std::min(chunk.startRow, n);
                                           const uint32_t end = std::min(chunk.endRow, n);

                                           for (uint32_t row = start; row < end; ++row)
                                           {
                                               auto &visibility = visibilityStates[row];
                                               const auto &bounds = renderBounds[row];

                                               stats.totalTested += 1u;

                                               const bool prevVisible = visibility.visible;
                                               visibility.wasVisibleLastFrame = prevVisible;

                                               const bool nowVisible = frustum.testSphere(bounds.worldCenter, bounds.worldRadius);
                                               visibility.visible = nowVisible;
                                               visibility.lastTestFrame = m_frameCounter;

                                               if (nowVisible)
                                               {
                                                   visibility.visibleFrame = m_frameCounter;
                                                   stats.visibleNow += 1u;
                                                   if (!prevVisible)
                                                       stats.becameVisible += 1u;
                                               }
                                               else if (prevVisible)
                                               {
                                                   stats.becameInvisible += 1u;
                                               }

                                               if (nowVisible != prevVisible)
                                               {
                                                   RowRef rr;
                                                   rr.archetypeId = chunk.archetypeId;
                                                   rr.row = row;
                                                   changed.push_back(rr);
                                               }
                                           }
                                       });

            // Reduce stats and apply dirties on the main thread.
            m_lastStats = Stats{};
            for (uint32_t i = 0u; i < scratchCount; ++i)
            {
                m_lastStats.totalTested += m_workerStats[i].totalTested;
                m_lastStats.visibleNow += m_workerStats[i].visibleNow;
                m_lastStats.becameVisible += m_workerStats[i].becameVisible;
                m_lastStats.becameInvisible += m_workerStats[i].becameInvisible;

                for (const RowRef &rr : m_workerChanged[i])
                {
                    ecs.markDirty(m_visibilityStateId, rr.archetypeId, rr.row);
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
                if (!store.hasRenderBounds() || !store.hasVisibilityState())
                    continue;

                auto &renderBounds = store.renderBounds();
                auto &visibilityStates = store.visibilityState();
                const uint32_t n = store.size();

                for (uint32_t row = 0; row < n; ++row)
                {
                    auto &visibility = visibilityStates[row];
                    const auto &bounds = renderBounds[row];

                    m_lastStats.totalTested += 1u;

                    // Store previous visibility for transition detection
                    const bool prevVisible = visibility.visible;
                    visibility.wasVisibleLastFrame = prevVisible;

                    // Test sphere against frustum
                    visibility.visible = frustum.testSphere(bounds.worldCenter, bounds.worldRadius);

                    // Track when visibility changes
                    visibility.lastTestFrame = m_frameCounter;

                    if (visibility.visible)
                    {
                        visibility.visibleFrame = m_frameCounter;
                        m_lastStats.visibleNow += 1u;
                        if (!prevVisible)
                            m_lastStats.becameVisible += 1u;
                    }
                    else if (prevVisible)
                    {
                        m_lastStats.becameInvisible += 1u;
                    }

                    if (visibility.visible != prevVisible)
                    {
                        ecs.markDirty(m_visibilityStateId, archetypeId, row);
                    }
                }
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            std::cout << "[VisibilityCullingSystem] frame=" << m_frameCounter
                      << " tested=" << m_lastStats.totalTested
                      << " visible=" << m_lastStats.visibleNow
                      << " becameVisible=" << m_lastStats.becameVisible
                      << " becameInvisible=" << m_lastStats.becameInvisible
                      << "\n";
        }
#endif
    }

    uint32_t getFrameCounter() const { return m_frameCounter; }

private:
    struct Chunk
    {
        uint32_t archetypeId = UINT32_MAX;
        uint32_t startRow = 0;
        uint32_t endRow = 0;
    };

    struct RowRef
    {
        uint32_t archetypeId = UINT32_MAX;
        uint32_t row = UINT32_MAX;
    };

    struct Stats
    {
        uint32_t totalTested = 0;
        uint32_t visibleNow = 0;
        uint32_t becameVisible = 0;
        uint32_t becameInvisible = 0;
    };

    Engine::Camera *m_camera = nullptr;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_visibilityStateId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_frameCounter = 0;
    Stats m_lastStats{};

    std::vector<Chunk> m_chunks;
    std::vector<Stats> m_workerStats;
    std::vector<std::vector<RowRef>> m_workerChanged;
};
