#pragma once
/*
  Query.h
  -------
  Purpose:
    - Provide a compiled ECS query: required/excluded masks + cached matching archetype IDs.

  Notes:
    - Queries cache matching archetype IDs to avoid scanning all stores.
    - Queries can optionally track dirty rows (bitset per matching store) so systems can update incrementally.
    - QueryManager incrementally updates store lists when new archetype stores are created.
*/

#include <atomic>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "ECS/Components.h"

namespace Engine::ECS
{
  using QueryId = uint32_t;

  struct Query
  {
    // Movable wrapper around an atomic 64-bit word.
    // std::atomic is not movable, but we want dirty bitsets to live inside std::vector.
    struct AtomicWord
    {
      std::atomic<uint64_t> v{0ull};
      AtomicWord() = default;
      explicit AtomicWord(uint64_t x) : v(x) {}
      AtomicWord(const AtomicWord &) = delete;
      AtomicWord &operator=(const AtomicWord &) = delete;
      AtomicWord(AtomicWord &&other) noexcept
      {
        v.store(other.v.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.v.store(0ull, std::memory_order_relaxed);
      }
      AtomicWord &operator=(AtomicWord &&other) noexcept
      {
        if (this == &other)
          return *this;
        v.store(other.v.load(std::memory_order_relaxed), std::memory_order_relaxed);
        other.v.store(0ull, std::memory_order_relaxed);
        return *this;
      }
    };

    Query() = default;
    Query(const Query &) = delete;
    Query &operator=(const Query &) = delete;

    Query(Query &&other) noexcept
        : required(other.required),
          excluded(other.excluded),
          matchingArchetypeIds(std::move(other.matchingArchetypeIds)),
          dirtyEnabled(other.dirtyEnabled),
          dirtyComponents(other.dirtyComponents),
          archetypeToMatchIndex(std::move(other.archetypeToMatchIndex)),
          dirtyBits(std::move(other.dirtyBits))
    {
      other.dirtyEnabled = false;
    }

    Query &operator=(Query &&other) noexcept
    {
      if (this == &other)
        return *this;

      required = other.required;
      excluded = other.excluded;
      matchingArchetypeIds = std::move(other.matchingArchetypeIds);
      dirtyEnabled = other.dirtyEnabled;
      dirtyComponents = other.dirtyComponents;
      archetypeToMatchIndex = std::move(other.archetypeToMatchIndex);
      dirtyBits = std::move(other.dirtyBits);

      other.dirtyEnabled = false;
      return *this;
    }

    ComponentMask required;
    ComponentMask excluded;
    std::vector<uint32_t> matchingArchetypeIds;

    // Dirty tracking (optional)
    bool dirtyEnabled = false;
    ComponentMask dirtyComponents;

    // For O(1) lookup of a matching archetype index.
    std::unordered_map<uint32_t, uint32_t> archetypeToMatchIndex;

    // Parallel to matchingArchetypeIds: bitset per matching archetype.
    // Row i is dirty if (dirtyBits[matchIdx][i/64] & (1ull<<(i%64))) != 0.

    // Each word is atomic so markDirty and consumeDirtyRows can be used safely from multiple threads
    // as long as the query list / archetype matching list is not being structurally modified concurrently.
    std::vector<std::vector<AtomicWord>> dirtyBits;

    // Protects rare structural changes to dirtyBits (resizing when store size grows).
    mutable std::mutex dirtyResizeMutex;
  };
}
