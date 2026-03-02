#pragma once
/*
  SpatialIndexSystem.h
  --------------------
  Purpose:
        - Build a spatial hash grid (cellSize = R) of all entities that have Position.
        - Uses gameplay world coordinates in meters.
            Ground plane is X/Z (Y is height).
        - Enable fast neighbor lookups by querying only the 3×3 neighborhood of a cell.

  Usage:
    - Construct the system, setCellSize(R), and call buildMasks(registry) once (requires "Position").
    - Call update(stores, dt) each frame to rebuild the grid.
    - LocalAvoidanceSystem (or other systems) can call forNeighbors(x, y, fn) to visit candidate neighbors.

  Notes:
    - This is stateless across frames: we rebuild the grid each frame (simple and fast for RTS scales).
    - The grid stores (storeId, row) pairs so you can access components back in ArchetypeStoreManager.
*/

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"

#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct GridKey
{
    int gx = 0;
    int gz = 0;
    bool operator==(const GridKey &o) const noexcept { return gx == o.gx && gz == o.gz; }
};

struct GridKeyHash
{
    std::size_t operator()(const GridKey &k) const noexcept
    {
        // 64-bit mix (works fine for typical ranges of grid indices)
        // Constants are primes often used for 2D hashing.
        const uint64_t x = static_cast<uint64_t>(static_cast<int64_t>(k.gx));
        const uint64_t z = static_cast<uint64_t>(static_cast<int64_t>(k.gz));
        uint64_t h = 1469598103934665603ull; // FNV offset basis
        h ^= (x * 1099511628211ull);
        h *= 1469598103934665603ull;
        h ^= (z * 1099511628211ull);
        h *= 1469598103934665603ull;
        return static_cast<std::size_t>(h);
    }
};

struct GridEntry
{
    uint32_t storeId; // index into ArchetypeStoreManager::stores()
    uint32_t row;     // row within that store
};

struct GridCell
{
    std::vector<GridEntry> entries;
};

class SpatialIndexSystem : public Engine::ECS::SystemBase
{
public:
    SpatialIndexSystem(float cellSize = 2.0f) // default R in meters; adjust at runtime as needed
        : m_cellSize(cellSize)
    {
        setRequiredNames({"Position"}); // we index any entity that has Position
        // You may set excluded tags if desired: setExcludedNames({"Disabled","Dead"});
    }

    const char *name() const override { return "SpatialIndexSystem"; }

    void setCellSize(float cellSize) { m_cellSize = (cellSize > 1e-6f) ? cellSize : 1e-6f; }
    float getCellSize() const { return m_cellSize; }

    // Rebuild the spatial hash grid for all entities with Position
    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        // Clear grid but keep capacity to minimize allocations
        for (auto &kv : m_grid)
            kv.second.entries.clear();
        // Optional: if entity count changes wildly, you can occasionally m_grid.clear()

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            const Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            const auto &store = *storePtr;
            if (!store.hasPosition())
                continue;

            const auto &positions = store.positions();
            const uint32_t n = store.size();
            for (uint32_t row = 0; row < n; ++row)
            {
                const auto &p = positions[row];
                const int gx = static_cast<int>(std::floor(p.x / m_cellSize));
                const int gz = static_cast<int>(std::floor(p.z / m_cellSize));
                GridKey key{gx, gz};
                auto &cell = m_grid[key];
                cell.entries.push_back(GridEntry{archetypeId, row});
            }
        }
    }

    // Visit candidate neighbors around (x,z): we scan the 3×3 neighborhood (cell, plus its 8 adjacent cells).
    // Visitor signature: void(uint32_t storeId, uint32_t row)
    template <typename Visitor>
    void forNeighbors(float x, float z, Visitor &&visit) const
    {
        const int gx = static_cast<int>(std::floor(x / m_cellSize));
        const int gz = static_cast<int>(std::floor(z / m_cellSize));
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                const GridKey key{gx + dx, gz + dy};
                auto it = m_grid.find(key);
                if (it == m_grid.end())
                    continue;
                for (const auto &e : it->second.entries)
                    visit(e.storeId, e.row);
            }
        }
    }

    // Optional: expose direct cell access if needed
    const std::unordered_map<GridKey, GridCell, GridKeyHash> &grid() const { return m_grid; }

private:
    float m_cellSize; // equals neighbor radius R
    std::unordered_map<GridKey, GridCell, GridKeyHash> m_grid;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
};
