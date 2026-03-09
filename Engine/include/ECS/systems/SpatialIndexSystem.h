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

#include "utils/JobSystem.h"

#include <cmath>
#include <cstdint>
#include <algorithm>
#include <utility>
#include <vector>

struct GridKey
{
    int gx = 0;
    int gz = 0;
    bool operator==(const GridKey &o) const noexcept { return gx == o.gx && gz == o.gz; }
};

struct GridKeyLess
{
    bool operator()(const GridKey &a, const GridKey &b) const noexcept
    {
        if (a.gx != b.gx)
            return a.gx < b.gx;
        return a.gz < b.gz;
    }
};

struct GridEntry
{
    uint32_t storeId; // index into ArchetypeStoreManager::stores()
    uint32_t row;     // row within that store
};

struct KeyedGridEntry
{
    GridKey key;
    GridEntry entry;
};

struct GridCellRange
{
    GridKey key;
    uint32_t start = 0;
    uint32_t count = 0;
};

class SpatialIndexSystem : public Engine::ECS::SystemBase
{
public:
    // =====================
    // TUNING CONSTANTS
    // =====================
    static constexpr uint32_t PARALLEL_ENTRY_THRESHOLD = 4096; // parallelize when there are enough entities to index
    static constexpr uint32_t PARALLEL_CHUNK_SIZE = 1024;      // rows per chunk task

    SpatialIndexSystem(float cellSize = 2.0f) // default R in meters; adjust at runtime as needed
        : m_cellSize(cellSize)
    {
        setRequiredNames({"Position"}); // we index any entity that has Position
        // You may set excluded tags if desired: setExcludedNames({"Disabled","Dead"});
    }

    const char *name() const override { return "SpatialIndexSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void setCellSize(float cellSize) { m_cellSize = (cellSize > 1e-6f) ? cellSize : 1e-6f; }
    float getCellSize() const { return m_cellSize; }

    // Debug/telemetry: counts from the most recent rebuild.
    // - entriesIndexed: number of (store,row) pairs inserted (typically equals total entities with Position).
    // - cellsBuilt: number of unique spatial cells created (unique GridKey count).
    uint32_t lastEntriesIndexed() const { return m_lastEntriesIndexed; }
    uint32_t lastCellsBuilt() const { return m_lastCellsBuilt; }

    // Rebuild the spatial hash grid for all entities with Position
    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        m_lastEntriesIndexed = 0;
        m_lastCellsBuilt = 0;

        // Option C: Flat list of entries, sorted by GridKey, then compacted into ranges.
        // This avoids unordered_map contention and is friendly to parallel rebuilds.
        m_entries.clear();
        m_cells.clear();

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);

        auto addStoreEntries = [&](std::vector<KeyedGridEntry> &out, uint32_t archetypeId, uint32_t startRow, uint32_t count)
        {
            const Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                return;
            const auto &store = *storePtr;
            if (!store.hasPosition())
                return;

            const auto &positions = store.positions();
            const uint32_t n = store.size();
            if (startRow >= n)
                return;
            const uint32_t endRow = std::min(n, startRow + count);

            out.reserve(out.size() + (endRow - startRow));
            for (uint32_t row = startRow; row < endRow; ++row)
            {
                const auto &p = positions[row];
                const int gx = static_cast<int>(std::floor(p.x / m_cellSize));
                const int gz = static_cast<int>(std::floor(p.z / m_cellSize));
                out.push_back(KeyedGridEntry{GridKey{gx, gz}, GridEntry{archetypeId, row}});
            }
        };

        Engine::JobSystem *js = ecs.jobSystem;
        const bool hasWorkers = (js != nullptr) && (js->workerCount() > 0);

        // Pre-scan stores to estimate entity count and build chunk tasks.
        m_chunks.clear();
        uint32_t totalEntities = 0;
        if (hasWorkers)
        {
            for (uint32_t archetypeId : q.matchingArchetypeIds)
            {
                const Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
                if (!storePtr)
                    continue;
                const auto &store = *storePtr;
                if (!store.hasPosition())
                    continue;
                const uint32_t n = store.size();
                totalEntities += n;
                for (uint32_t start = 0; start < n; start += PARALLEL_CHUNK_SIZE)
                {
                    const uint32_t c = std::min(PARALLEL_CHUNK_SIZE, n - start);
                    m_chunks.push_back(StoreChunk{archetypeId, start, c});
                }
            }
        }

        const bool canParallel = hasWorkers && (totalEntities >= PARALLEL_ENTRY_THRESHOLD) && !m_chunks.empty();

        if (canParallel)
        {
            const uint32_t scratchCount = js->workerCount() + 1; // + calling thread
            if (m_workerScratch.size() != scratchCount)
                m_workerScratch.resize(scratchCount);
            for (auto &v : m_workerScratch)
                v.clear();

            js->parallelFor(static_cast<uint32_t>(m_chunks.size()),
                            [&](uint32_t workerIndex, uint32_t i)
                            {
                                if (i >= m_chunks.size())
                                    return;
                                const StoreChunk &c = m_chunks[i];
                                addStoreEntries(m_workerScratch[workerIndex], c.archetypeId, c.startRow, c.count);
                            });

            size_t total = 0;
            for (const auto &v : m_workerScratch)
                total += v.size();
            m_entries.reserve(total);
            for (const auto &v : m_workerScratch)
                m_entries.insert(m_entries.end(), v.begin(), v.end());
        }
        else
        {
            // Sequential fallback.
            for (uint32_t archetypeId : q.matchingArchetypeIds)
            {
                const Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
                if (!storePtr)
                    continue;
                const auto &store = *storePtr;
                if (!store.hasPosition())
                    continue;
                addStoreEntries(m_entries, archetypeId, 0u, store.size());
            }
        }

        if (m_entries.empty())
            return;

        // Sort by cell key, then by entry for determinism.
        std::sort(m_entries.begin(), m_entries.end(), [](const KeyedGridEntry &a, const KeyedGridEntry &b)
                  {
                      GridKeyLess less;
                      if (less(a.key, b.key))
                          return true;
                      if (less(b.key, a.key))
                          return false;
                      if (a.entry.storeId != b.entry.storeId)
                          return a.entry.storeId < b.entry.storeId;
                      return a.entry.row < b.entry.row; });

        // Compact contiguous ranges per cell.
        m_cells.reserve(m_entries.size() / 4 + 1);
        GridKeyLess less;
        uint32_t start = 0;
        for (uint32_t i = 1; i < static_cast<uint32_t>(m_entries.size()); ++i)
        {
            const GridKey &prev = m_entries[start].key;
            const GridKey &cur = m_entries[i].key;
            const bool different = less(prev, cur) || less(cur, prev);
            if (different)
            {
                m_cells.push_back(GridCellRange{prev, start, i - start});
                start = i;
            }
        }
        m_cells.push_back(GridCellRange{m_entries[start].key, start, static_cast<uint32_t>(m_entries.size()) - start});

        m_lastEntriesIndexed = static_cast<uint32_t>(m_entries.size());
        m_lastCellsBuilt = static_cast<uint32_t>(m_cells.size());
    }

    // Visit candidate neighbors around (x,z): we scan the 3×3 neighborhood (cell, plus its 8 adjacent cells).
    // Visitor signature: void(uint32_t storeId, uint32_t row)
    template <typename Visitor>
    void forNeighbors(float x, float z, Visitor &&visit) const
    {
        const int gx = static_cast<int>(std::floor(x / m_cellSize));
        const int gz = static_cast<int>(std::floor(z / m_cellSize));
        GridKeyLess less;
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                const GridKey key{gx + dx, gz + dy};
                auto it = std::lower_bound(m_cells.begin(), m_cells.end(), key,
                                           [&](const GridCellRange &cell, const GridKey &k)
                                           { return less(cell.key, k); });
                if (it == m_cells.end())
                    continue;
                if (less(key, it->key) || less(it->key, key))
                    continue;

                const uint32_t end = it->start + it->count;
                for (uint32_t i = it->start; i < end; ++i)
                {
                    const auto &e = m_entries[i].entry;
                    visit(e.storeId, e.row);
                }
            }
        }
    }

private:
    float m_cellSize; // equals neighbor radius R

    // Flat entry list, sorted by GridKey.
    std::vector<KeyedGridEntry> m_entries;

    // Sorted unique cells with contiguous ranges into m_entries.
    std::vector<GridCellRange> m_cells;

    // Per-worker scratch to build m_entries without contention.
    std::vector<std::vector<KeyedGridEntry>> m_workerScratch;

    struct StoreChunk
    {
        uint32_t archetypeId = 0;
        uint32_t startRow = 0;
        uint32_t count = 0;
    };
    std::vector<StoreChunk> m_chunks;

    uint32_t m_lastEntriesIndexed = 0;
    uint32_t m_lastCellsBuilt = 0;

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
};
