#pragma once
/*
  QueryManager.h
  --------------
  Purpose:
    - Own and manage compiled queries.
    - Cache matching archetype/store IDs so systems stop scanning all stores.

  v1 behavior:
    - createQuery(required, excluded) compiles the query against existing stores.
    - onStoreCreated(archetypeId, signature) incrementally updates all queries.
*/

#include <cstdint>
#include <vector>
#include <algorithm>
#include <mutex>
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include "ECS/Query.h"
#include "ECS/EcsTrace.h"
#include "ECS/ArchetypeStore.h"

namespace Engine::ECS
{
    class QueryManager
    {
    public:
        static constexpr QueryId InvalidQuery = UINT32_MAX;

        // Optional: ECS trace for debug profiling.
        void setTrace(EcsTrace *trace) { m_trace = trace; }

        // Set by the schedule driver (SystemRunner) so consumeDirtyRows can attribute work.
        void setCurrentSystemName(const char *name) { t_currentSystemName = name; }
        void clearCurrentSystemName() { t_currentSystemName = nullptr; }

        QueryId createQuery(const ComponentMask &required, const ComponentMask &excluded,
                            const ArchetypeStoreManager &mgr)
        {
            const QueryId id = static_cast<QueryId>(m_queries.size());
            Query q;
            q.required = required;
            q.excluded = excluded;

            // Compile against existing stores.
            const auto &stores = mgr.stores();
            for (uint32_t archetypeId = 0; archetypeId < stores.size(); ++archetypeId)
            {
                const auto &ptr = stores[archetypeId];
                if (!ptr)
                    continue;
                const auto &sig = ptr->signature();
                if (!sig.containsAll(required))
                    continue;
                if (!sig.containsNone(excluded))
                    continue;
                const uint32_t matchIdx = static_cast<uint32_t>(q.matchingArchetypeIds.size());
                q.matchingArchetypeIds.push_back(archetypeId);
                q.archetypeToMatchIndex.emplace(archetypeId, matchIdx);
            }

            m_queries.emplace_back(std::move(q));
            return id;
        }

        // Create a query that tracks dirty rows. Initially marks ALL existing rows in matching stores as dirty.
        QueryId createDirtyQuery(const ComponentMask &required, const ComponentMask &excluded,
                                 const ComponentMask &dirtyComponents,
                                 const ArchetypeStoreManager &mgr)
        {
            const QueryId id = createQuery(required, excluded, mgr);
            Query &q = m_queries[id];
            q.dirtyEnabled = true;
            q.dirtyComponents = dirtyComponents;

            q.dirtyBits.clear();
            q.dirtyBits.resize(q.matchingArchetypeIds.size());

            for (size_t i = 0; i < q.matchingArchetypeIds.size(); ++i)
            {
                const uint32_t archetypeId = q.matchingArchetypeIds[i];
                const ArchetypeStore *store = mgr.get(archetypeId);
                const uint32_t n = store ? store->size() : 0u;
                ensureBitsetSize(q, q.dirtyBits[i], n);
                // Mark all rows dirty.
                for (uint32_t row = 0; row < n; ++row)
                    setDirtyBit(q.dirtyBits[i], row);
            }

            return id;
        }

        const Query &get(QueryId id) const { return m_queries[id]; }

        void onStoreCreated(uint32_t archetypeId, const ComponentMask &signature)
        {
            for (auto &q : m_queries)
            {
                if (!signature.containsAll(q.required))
                    continue;
                if (!signature.containsNone(q.excluded))
                    continue;
                const uint32_t matchIdx = static_cast<uint32_t>(q.matchingArchetypeIds.size());
                q.matchingArchetypeIds.push_back(archetypeId);
                q.archetypeToMatchIndex.emplace(archetypeId, matchIdx);
                if (q.dirtyEnabled)
                {
                    q.dirtyBits.emplace_back();
                }
            }
        }

        // Mark a row dirty for any query that is interested in 'compId'.
        void markDirtyComponent(uint32_t compId, uint32_t archetypeId, uint32_t row, uint32_t storeSize)
        {
            for (auto &q : m_queries)
            {
                if (!q.dirtyEnabled)
                    continue;
                if (!q.dirtyComponents.has(compId))
                    continue;

                auto it = q.archetypeToMatchIndex.find(archetypeId);
                if (it == q.archetypeToMatchIndex.end())
                    continue;
                const uint32_t matchIdx = it->second;
                ensureBitsetSize(q, q.dirtyBits[matchIdx], storeSize);
                setDirtyBit(q.dirtyBits[matchIdx], row);
            }
        }

        // Mark a row dirty for ALL dirty-enabled queries that match the store.
        void markRowDirtyAll(uint32_t archetypeId, uint32_t row, uint32_t storeSize)
        {
            for (auto &q : m_queries)
            {
                if (!q.dirtyEnabled)
                    continue;
                auto it = q.archetypeToMatchIndex.find(archetypeId);
                if (it == q.archetypeToMatchIndex.end())
                    continue;
                const uint32_t matchIdx = it->second;
                ensureBitsetSize(q, q.dirtyBits[matchIdx], storeSize);
                setDirtyBit(q.dirtyBits[matchIdx], row);
            }
        }

        // Consume and clear dirty rows for a given query+archetype.
        // Returns row indices in ascending order.
        std::vector<uint32_t> consumeDirtyRows(QueryId qid, uint32_t archetypeId)
        {
            std::vector<uint32_t> rows;
            if (qid >= m_queries.size())
                return rows;
            Query &q = m_queries[qid];
            if (!q.dirtyEnabled)
                return rows;
            auto it = q.archetypeToMatchIndex.find(archetypeId);
            if (it == q.archetypeToMatchIndex.end())
                return rows;

            const uint32_t matchIdx = it->second;
            auto &bits = q.dirtyBits[matchIdx];
            for (size_t w = 0; w < bits.size(); ++w)
            {
                // Atomically consume this word: exchange to 0 so concurrent markDirty is safe.
                uint64_t word = bits[w].v.exchange(0ull, std::memory_order_acq_rel);
                if (word == 0)
                    continue;
                while (word)
                {
                    const uint64_t lsb = word & (~word + 1ull);
#ifdef _MSC_VER
                    unsigned long idx;
                    _BitScanForward64(&idx, word);
                    const uint32_t bit = static_cast<uint32_t>(idx);
#else
                    const uint32_t bit = static_cast<uint32_t>(__builtin_ctzll(word));
#endif
                    const uint32_t row = static_cast<uint32_t>(w * 64u + bit);
                    rows.push_back(row);
                    word ^= lsb;
                }
            }
            std::sort(rows.begin(), rows.end());

            if (m_trace && t_currentSystemName && !rows.empty())
            {
                m_trace->onDirtyConsumed(t_currentSystemName, archetypeId, static_cast<uint32_t>(rows.size()));
            }

            return rows;
        }

    private:
        std::vector<Query> m_queries;

        EcsTrace *m_trace = nullptr;

        static thread_local const char *t_currentSystemName;

        static void ensureBitsetSize(Query &q, std::vector<Query::AtomicWord> &bits, uint32_t rowCount)
        {
            const size_t needWords = static_cast<size_t>((rowCount + 63u) / 64u);
            if (bits.size() >= needWords)
                return;
            std::lock_guard<std::mutex> lock(q.dirtyResizeMutex);
            if (bits.size() < needWords)
                bits.resize(needWords);
        }

        static void setDirtyBit(std::vector<Query::AtomicWord> &bits, uint32_t row)
        {
            const size_t word = static_cast<size_t>(row / 64u);
            const uint32_t bit = row % 64u;
            if (word >= bits.size())
            {
                // Resize should be done by ensureBitsetSize (callers pass storeSize).
                bits.resize(word + 1);
            }
            bits[word].v.fetch_or((1ull << bit), std::memory_order_relaxed);
        }
    };
}
