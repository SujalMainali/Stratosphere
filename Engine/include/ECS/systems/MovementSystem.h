#pragma once
/*
  MovementSystem
  --------------
  Purpose:
    - Moves entities: position += velocity * dt for any store that has both Position and Velocity
      and does not contain excluded tags.
*/

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include "utils/JobSystem.h"

#include <cmath>
#include <algorithm>

class MovementSystem : public Engine::ECS::SystemBase
{
public:
    // =====================
    // TUNING CONSTANTS
    // =====================
    static constexpr uint32_t PARALLEL_DIRTY_ROW_THRESHOLD = 256;
    static constexpr float MAX_SPEED = 25.0f; // m/s, sanity cap
    static constexpr float MAX_STEP = 5.0f;   // m/frame, teleport guard

    MovementSystem()
    {
        setRequiredNames({"Position", "Velocity"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "MovementSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_velocityId = registry.ensureId("Velocity");
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (!(dt > 0.0f) || !std::isfinite(dt))
            return;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_velocityId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        auto finite3 = [](float a, float b, float c)
        { return std::isfinite(a) && std::isfinite(b) && std::isfinite(c); };

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            const auto &store = *storePtr;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            const uint32_t n = store.size();
            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            auto &velocities = const_cast<std::vector<Engine::ECS::Velocity> &>(store.velocities());

            auto processRow = [&](uint32_t i)
            {
                if (i >= n)
                    return;

                auto &pos = positions[i];
                auto &vel = velocities[i];

                if (!finite3(pos.x, pos.y, pos.z) || !finite3(vel.x, vel.y, vel.z))
                {
                    vel.x = vel.y = vel.z = 0.0f;
                    ecs.markDirty(m_velocityId, archetypeId, i);
                    return;
                }

                // Clamp absurd speeds so one bad frame can't launch an entity across the map.
                const float v2 = vel.x * vel.x + vel.y * vel.y + vel.z * vel.z;
                if (v2 > (MAX_SPEED * MAX_SPEED))
                {
                    const float inv = MAX_SPEED / std::sqrt(v2);
                    vel.x *= inv;
                    vel.y *= inv;
                    vel.z *= inv;
                }

                const float velMag1 = std::fabs(vel.x) + std::fabs(vel.y) + std::fabs(vel.z);
                if (velMag1 <= 1e-6f)
                    return;

                float dx = vel.x * dt;
                float dy = vel.y * dt;
                float dz = vel.z * dt;

                if (!finite3(dx, dy, dz))
                {
                    vel.x = vel.y = vel.z = 0.0f;
                    ecs.markDirty(m_velocityId, archetypeId, i);
                    return;
                }

                const float step2 = dx * dx + dy * dy + dz * dz;
                if (step2 > (MAX_STEP * MAX_STEP))
                {
                    const float inv = MAX_STEP / std::sqrt(step2);
                    dx *= inv;
                    dy *= inv;
                    dz *= inv;
                    // Keep velocity consistent with the clamped step.
                    vel.x = dx / dt;
                    vel.y = dy / dt;
                    vel.z = dz / dt;
                }

                const float oldX = pos.x;
                const float oldY = pos.y;
                const float oldZ = pos.z;

                pos.x += dx;
                pos.y += dy;
                pos.z += dz;

                if (!finite3(pos.x, pos.y, pos.z))
                {
                    pos.x = oldX;
                    pos.y = oldY;
                    pos.z = oldZ;
                    vel.x = vel.y = vel.z = 0.0f;
                    ecs.markDirty(m_velocityId, archetypeId, i);
                    ecs.markDirty(m_positionId, archetypeId, i);
                    return;
                }

                ecs.markDirty(m_positionId, archetypeId, i);

                // Keep movers active: movement must run every frame while velocity is non-zero.
                ecs.markDirty(m_velocityId, archetypeId, i);
            };

            // Parallelize over dirty rows when a job system is available and the batch is non-trivial.
            if (ecs.jobSystem && dirtyRows.size() >= PARALLEL_DIRTY_ROW_THRESHOLD)
            {
                ecs.jobSystem->parallelFor(static_cast<uint32_t>(dirtyRows.size()), [&](uint32_t /*worker*/, uint32_t item)
                                           { processRow(dirtyRows[item]); });
            }
            else
            {
                for (uint32_t i : dirtyRows)
                    processRow(i);
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
};
