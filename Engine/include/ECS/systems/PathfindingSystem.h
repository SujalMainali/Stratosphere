#pragma once
/*
  PathfindingSystem
  -----------------
  Purpose:
    - Finds A* path for units that have a MoveTarget but no valid Path.
    - Updates Path component with waypoints.
    - Only runs when needed (dirty query on MoveTarget).

  Optimizations:
    - Generation counter avoids clearing arrays per A* call.
    - Reusable member buffers avoid heap allocations per call.
    - Dirty query avoids scanning all entities every frame.
    - Capped lookahead in path smoothing avoids expensive line checks.
    - Weighted A* (ε=1.2) explores fewer nodes for near-optimal paths.
    - Grid-space lineCheck avoids float↔int conversions in smoothing.
    - Target cell validation with spiral fallback prevents wasted A* on blocked goals.
*/

#include "ECS/SystemFormat.h"
#include "ECS/systems/NavGrid.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class PathfindingSystem : public Engine::ECS::SystemBase
{
public:
    PathfindingSystem(const NavGrid *grid)
        : m_grid(grid)
    {
        setRequiredNames({"Position", "MoveTarget", "Path"});
        setExcludedNames({"Disabled", "Dead", "Obstacle"});
    }

    const char *name() const override { return "PathfindingSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_moveTargetId = registry.ensureId("MoveTarget");
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (!m_grid)
            return;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_moveTargetId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            auto &positions = store.positions();
            auto &targets = store.moveTargets();
            auto &paths = store.paths();
            const uint32_t n = store.size();

            for (uint32_t i : dirtyRows)
            {
                if (i >= n)
                    continue;

                auto &pos = positions[i];
                auto &tgt = targets[i];
                auto &path = paths[i];

                if (!tgt.active)
                {
                    path.valid = false;
                    path.count = 0;
                    path.current = 0;
                    continue;
                }

                // MoveTarget became dirty => treat this as a new goal and replan.
                // (SteeringSystem no longer marks MoveTarget dirty each frame.)
                path.valid = false;
                path.count = 0;
                path.current = 0;

                const float oldTx = tgt.x;
                const float oldTz = tgt.z;

                runAStar(pos, tgt, path);

                if (tgt.x != oldTx || tgt.z != oldTz)
                {
                    // Keep other systems (and future frames) aware that the goal changed.
                    ecs.markDirty(m_moveTargetId, archetypeId, i);
                }
            }
        }
    }

private:
    const NavGrid *m_grid;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;

    uint32_t m_currentGen = 0;
    std::vector<uint32_t> m_genStamp;
    std::vector<float> m_gScores;
    std::vector<int> m_cameFrom;
    std::vector<uint32_t> m_closedGen;

    struct NodeEntry
    {
        int idx;
        float fCost;
        bool operator>(const NodeEntry &o) const { return fCost > o.fCost; }
    };
    std::vector<NodeEntry> m_heapBuf;
    std::vector<int> m_pathIndices;
    std::vector<int> m_smoothedIdx;

    static constexpr float kEpsilon = 1.2f;

    float heuristic(int x1, int z1, int x2, int z2) const
    {
        int dx = std::abs(x1 - x2);
        int dz = std::abs(z1 - z2);
        return static_cast<float>(std::max(dx, dz)) + 0.414f * static_cast<float>(std::min(dx, dz));
    }

    void ensureGridBuffers()
    {
        const size_t gridSize = static_cast<size_t>(m_grid->width) * m_grid->height;
        if (m_genStamp.size() < gridSize)
        {
            m_genStamp.resize(gridSize, 0);
            m_gScores.resize(gridSize, 1e9f);
            m_cameFrom.resize(gridSize, -1);
            m_closedGen.resize(gridSize, 0);
        }
    }

    bool isVisited(int idx) const { return m_genStamp[idx] == m_currentGen; }
    bool isClosed(int idx) const { return m_closedGen[idx] == m_currentGen; }

    float getG(int idx) const
    {
        return isVisited(idx) ? m_gScores[idx] : 1e9f;
    }

    void setG(int idx, float g, int parent)
    {
        m_genStamp[idx] = m_currentGen;
        m_gScores[idx] = g;
        m_cameFrom[idx] = parent;
    }

    void setClosed(int idx)
    {
        m_closedGen[idx] = m_currentGen;
    }

    void runAStar(const Engine::ECS::Position &startPos, Engine::ECS::MoveTarget &target, Engine::ECS::Path &outPath)
    {
        const int W = m_grid->width;
        const int H = m_grid->height;

        auto idx = [W](int x, int z)
        { return z * W + x; };
        auto idxToX = [W](int i)
        { return i % W; };
        auto idxToZ = [W](int i)
        { return i / W; };

        const int startX = m_grid->worldToGridX(startPos.x);
        const int startZ = m_grid->worldToGridZ(startPos.z);
        int targetX = std::max(0, std::min(W - 1, m_grid->worldToGridX(target.x)));
        int targetZ = std::max(0, std::min(H - 1, m_grid->worldToGridZ(target.z)));

        bool relocatedTarget = false;

        if (!m_grid->isWalkable(targetX, targetZ))
        {
            bool relocated = false;
            for (int r = 1; r <= 10 && !relocated; ++r)
            {
                for (int dx = -r; dx <= r && !relocated; ++dx)
                {
                    for (int dz = -r; dz <= r && !relocated; ++dz)
                    {
                        if (std::abs(dx) != r && std::abs(dz) != r)
                            continue;
                        int nx = targetX + dx, nz = targetZ + dz;
                        if (m_grid->isWalkable(nx, nz))
                        {
                            targetX = nx;
                            targetZ = nz;
                            relocated = true;
                        }
                    }
                }
            }
            if (!relocated)
            {
                outPath.valid = false;
                return;
            }

            relocatedTarget = true;
        }

        // If we had to relocate the goal cell, also relocate the entity's MoveTarget in world-space.
        // Otherwise the SteeringSystem will eventually fall back to the original (blocked) MoveTarget
        // after consuming the path, which causes oscillation/chaos near obstacle fields.
        if (relocatedTarget)
        {
            target.x = m_grid->gridToWorldX(targetX);
            target.z = m_grid->gridToWorldZ(targetZ);
        }

        const int startIdx = idx(startX, startZ);
        const int targetIdx = idx(targetX, targetZ);

        if (startIdx == targetIdx)
        {
            outPath.valid = true;
            outPath.count = 0;
            outPath.current = 0;
            return;
        }

        if (m_grid->lineCheckGrid(startX, startZ, targetX, targetZ))
        {
            outPath.valid = true;
            outPath.count = 0;
            outPath.current = 0;
            return;
        }

        ensureGridBuffers();
        ++m_currentGen;
        if (m_currentGen == 0)
        {
            std::fill(m_genStamp.begin(), m_genStamp.end(), 0u);
            std::fill(m_closedGen.begin(), m_closedGen.end(), 0u);
            m_currentGen = 1;
        }

        m_heapBuf.clear();
        if (m_heapBuf.capacity() < 256)
            m_heapBuf.reserve(256);

        auto heapPush = [&](int cellIdx, float f)
        {
            m_heapBuf.push_back({cellIdx, f});
            std::push_heap(m_heapBuf.begin(), m_heapBuf.end(), std::greater<NodeEntry>{});
        };
        auto heapPop = [&]() -> NodeEntry
        {
            std::pop_heap(m_heapBuf.begin(), m_heapBuf.end(), std::greater<NodeEntry>{});
            NodeEntry n = m_heapBuf.back();
            m_heapBuf.pop_back();
            return n;
        };

        setG(startIdx, 0.0f, -1);
        const float startH = heuristic(startX, startZ, targetX, targetZ);
        heapPush(startIdx, kEpsilon * startH);

        bool found = false;
        int closestIdx = startIdx;
        float closestH = startH;

        int nodesExplored = 0;
        constexpr int MAX_NODES = 4000;

        static constexpr int dxAddr[] = {0, 0, -1, 1, -1, -1, 1, 1};
        static constexpr int dzAddr[] = {-1, 1, 0, 0, -1, 1, -1, 1};
        static constexpr float costs[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.414f, 1.414f, 1.414f, 1.414f};

        while (!m_heapBuf.empty())
        {
            const NodeEntry current = heapPop();

            if (isClosed(current.idx))
                continue;
            setClosed(current.idx);

            if (++nodesExplored > MAX_NODES)
                break;

            if (current.idx == targetIdx)
            {
                found = true;
                break;
            }

            const float curG = getG(current.idx);
            const float curH = (current.fCost / kEpsilon) - curG + 0.001f;
            if (curH < closestH)
            {
                closestH = curH;
                closestIdx = current.idx;
            }

            const int cx = idxToX(current.idx);
            const int cz = idxToZ(current.idx);

            for (int i = 0; i < 8; ++i)
            {
                const int nx = cx + dxAddr[i];
                const int nz = cz + dzAddr[i];

                if (nx < 0 || nx >= W || nz < 0 || nz >= H)
                    continue;

                const int nIdx = idx(nx, nz);
                if (m_grid->blocked[nIdx])
                    continue;
                if (isClosed(nIdx))
                    continue;

                if (i >= 4)
                {
                    if (m_grid->blocked[idx(cx, nz)] || m_grid->blocked[idx(nx, cz)])
                        continue;
                }

                const float newG = curG + costs[i];

                if (newG < getG(nIdx))
                {
                    setG(nIdx, newG, current.idx);
                    const float h = heuristic(nx, nz, targetX, targetZ);
                    heapPush(nIdx, newG + kEpsilon * h);
                }
            }
        }

        int backIdx = found ? targetIdx : closestIdx;
        m_pathIndices.clear();

        while (backIdx != startIdx)
        {
            m_pathIndices.push_back(backIdx);
            const int pIdx = m_cameFrom[backIdx];
            if (pIdx < 0)
                break;
            backIdx = pIdx;
            if (m_pathIndices.size() > 200)
                break;
        }

        std::reverse(m_pathIndices.begin(), m_pathIndices.end());

        m_smoothedIdx.clear();
        m_smoothedIdx.reserve(m_pathIndices.size() + 1);

        constexpr size_t kMaxLookahead = 16;

        int anchorX = startX, anchorZ = startZ;
        size_t pi = 0;

        while (pi < m_pathIndices.size())
        {
            size_t bestAdvance = pi;
            const size_t maxCheck = std::min(pi + kMaxLookahead + 1, m_pathIndices.size());

            for (size_t j = pi + 1; j < maxCheck; ++j)
            {
                const int jx = idxToX(m_pathIndices[j]);
                const int jz = idxToZ(m_pathIndices[j]);
                if (!m_grid->lineCheckGrid(anchorX, anchorZ, jx, jz))
                    break;
                bestAdvance = j;
            }

            const int chosen = m_pathIndices[bestAdvance];
            m_smoothedIdx.push_back(chosen);
            anchorX = idxToX(chosen);
            anchorZ = idxToZ(chosen);
            pi = bestAdvance + 1;
        }

        if (!m_smoothedIdx.empty() && !m_pathIndices.empty() &&
            m_smoothedIdx.back() != m_pathIndices.back())
        {
            m_smoothedIdx.push_back(m_pathIndices.back());
        }

        outPath.count = 0;
        outPath.current = 0;

        for (size_t si = 0; si < m_smoothedIdx.size(); ++si)
        {
            if (outPath.count >= Engine::ECS::Path::MAX_WAYPOINTS)
                break;

            const bool isLast = (si == m_smoothedIdx.size() - 1);
            if (isLast)
            {
                outPath.waypointsX[outPath.count] = target.x;
                outPath.waypointsZ[outPath.count] = target.z;
            }
            else
            {
                outPath.waypointsX[outPath.count] = m_grid->gridToWorldX(idxToX(m_smoothedIdx[si]));
                outPath.waypointsZ[outPath.count] = m_grid->gridToWorldZ(idxToZ(m_smoothedIdx[si]));
            }
            outPath.count++;
        }

        outPath.valid = true;
    }
};
