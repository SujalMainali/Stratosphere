#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include "utils/JobSystem.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>

// Updates Engine::ECS::RenderTransform from Position (+ optional Facing).
// Uses dirty queries so it only runs when Position/Facing are marked dirty.
class RenderTransformUpdateSystem : public Engine::ECS::SystemBase
{
public:
    // =====================
    // TUNING CONSTANTS
    // =====================
    static constexpr uint32_t PARALLEL_DIRTY_ROW_THRESHOLD = 256;

    RenderTransformUpdateSystem()
    {
        setRequiredNames({"Position", "RenderTransform"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderTransformUpdateSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_facingId = registry.ensureId("Facing");
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_positionId);
            dirty.set(m_facingId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            if (!store.hasPosition() || !store.hasRenderTransform())
                continue;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);

            const uint32_t n = store.size();
            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            auto &transforms = const_cast<std::vector<Engine::ECS::RenderTransform> &>(store.renderTransforms());
            const bool hasFacing = store.hasFacing();
            auto &facings = hasFacing ? const_cast<std::vector<Engine::ECS::Facing> &>(store.facings()) : m_dummyFacings;

            // Dirty-driven update is great once the system is running, but newly spawned entities may not have
            // their Position/Facing dirtied yet. Ensure we compute the initial world matrix once.
            if (dirtyRows.empty())
            {
                dirtyRows.reserve(n);
                for (uint32_t row = 0; row < n; ++row)
                {
                    if (transforms[row].transformVersion == 0)
                        dirtyRows.push_back(row);
                }
                if (dirtyRows.empty())
                    continue;
            }

            auto processRow = [&](uint32_t row)
            {
                if (row >= n)
                    return;

                const auto &pos = positions[row];
                const float yaw = hasFacing ? facings[row].yaw : 0.0f;

                if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z))
                    return;
                if (hasFacing && !std::isfinite(yaw))
                    return;

                glm::mat4 world = glm::translate(glm::mat4(1.0f), glm::vec3(pos.x, pos.y, pos.z));
                if (hasFacing)
                    world = glm::rotate(world, yaw, glm::vec3(0.0f, 1.0f, 0.0f));

                auto &rt = transforms[row];
                rt.world = world;
                rt.transformVersion += 1u;
            };

            if (ecs.jobSystem && dirtyRows.size() >= PARALLEL_DIRTY_ROW_THRESHOLD)
            {
                ecs.jobSystem->parallelFor(static_cast<uint32_t>(dirtyRows.size()), [&](uint32_t /*worker*/, uint32_t item)
                                           { processRow(dirtyRows[item]); });
            }
            else
            {
                for (uint32_t row : dirtyRows)
                    processRow(row);
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;

    std::vector<Engine::ECS::Facing> m_dummyFacings;
};
