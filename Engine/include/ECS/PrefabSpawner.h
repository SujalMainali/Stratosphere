#pragma once
/*
  PrefabSpawner.h
  ---------------
  Purpose:
    - Provide a function to spawn an entity from a Prefab:
      * Create an Entity (EntitiesRecord)
      * Create/get the matching ArchetypeStore
      * Create a row in the store, apply defaults
      * Attach the entity mapping for quick lookup

  Usage:
    - SpawnResult res = spawnFromPrefab(prefab, registry, archetypes, storeMgr, entities);
*/

#include "ECS/Prefab.h"
#include "ECS/ArchetypeManager.h"
#include "ECS/ArchetypeStore.h"
#include "ECS/Entity.h"
#include "ECS/Components.h"

namespace Engine::ECS
{
    // Result of spawning: includes entity handle, row index, and archetypeId.
    struct SpawnResult
    {
        Entity entity{};
        uint32_t row = UINT32_MAX;
        uint32_t archetypeId = UINT32_MAX;
    };

    inline SpawnResult spawnFromPrefab(const Prefab &prefab,
                                       ComponentRegistry &registry,
                                       ArchetypeManager &archetypes,
                                       ArchetypeStoreManager &stores,
                                       EntitiesRecord &entities)
    {
        SpawnResult res{};
        res.archetypeId = prefab.archetypeId;

        // Create entity
        res.entity = entities.create();

        // Get or create store for this archetype signature
        ArchetypeStore *store = stores.getOrCreate(prefab.archetypeId, prefab.signature, registry);

        // Create row and apply defaults
        res.row = store->createRow(res.entity);
        store->applyDefaults(res.row, prefab.defaults, registry);

        // Attach entity to record for quick per-entity operations
        entities.attach(res.entity, res.archetypeId, res.row);

        return res;
    }

} // namespace Engine::ECS