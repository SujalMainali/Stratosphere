#pragma once
#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include <algorithm>
#include <cmath>

class SteeringSystem : public Engine::ECS::SystemBase
{
public:
    SteeringSystem()
    {
        // Position + Velocity + MoveTarget + MoveSpeed + Path + Facing required
        setRequiredNames({"Position", "Velocity", "MoveTarget", "MoveSpeed", "Path", "Facing"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "SteeringSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_velocityId = registry.ensureId("Velocity");
        m_moveTargetId = registry.ensureId("MoveTarget");
        m_facingId = registry.ensureId("Facing");
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            // Re-steer when the target changes, and also when position changes (movement).
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_positionId);
            dirty.set(m_moveTargetId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        auto dist2 = [](float x, float z)
        { return x * x + z * z; };

        // Gameplay uses meters; stop once we're within a small radius.
        const float arrivalRadius2 = 0.25f;    // 0.5^2
        const float waypointRadius2 = 0.0625f; // 0.25^2

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

            auto &positions = const_cast<std::vector<Engine::ECS::Position> &>(store.positions());
            auto &velocities = const_cast<std::vector<Engine::ECS::Velocity> &>(store.velocities());
            auto &targets = const_cast<std::vector<Engine::ECS::MoveTarget> &>(store.moveTargets());
            auto &speeds = const_cast<std::vector<Engine::ECS::MoveSpeed> &>(store.moveSpeeds());
            auto &paths = const_cast<std::vector<Engine::ECS::Path> &>(store.paths());
            auto &facings = const_cast<std::vector<Engine::ECS::Facing> &>(store.facings());

            const uint32_t n = store.size();

            for (uint32_t i : dirtyRows)
            {
                if (i >= n)
                    continue;

                auto &pos = positions[i];
                auto &vel = velocities[i];
                auto &tgt = targets[i];
                const auto &spd = speeds[i];
                auto &path = paths[i];
                auto &facing = facings[i];

                if (!tgt.active)
                {
                    // No active movement intent. Whoever deactivated MoveTarget is responsible
                    // for clearing velocity if a hard stop is desired (e.g., arrival/combat).
                    continue;
                }

                // Determine target position: waypoint or final target
                float tx = tgt.x;
                float tz = tgt.z;
                bool isFinal = true;

                if (path.valid && path.current < path.count)
                {
                    tx = path.waypointsX[path.current];
                    tz = path.waypointsZ[path.current];
                    isFinal = false;
                }

                float dx = tx - pos.x;
                float dz = tz - pos.z;
                float d2 = dist2(dx, dz);

                // Check arrival (squared distance)
                float radiusToCheck2 = isFinal ? arrivalRadius2 : waypointRadius2;

                if (d2 <= radiusToCheck2)
                {
                    if (isFinal)
                    {
                        // Arrived at final destination
                        vel.x = vel.y = vel.z = 0.0f;
                        tgt.active = 0;
                        path.valid = false;
                    }
                    else
                    {
                        // Arrived at waypoint — advance to next
                        path.current++;
                        if (path.current < path.count)
                        {
                            tx = path.waypointsX[path.current];
                            tz = path.waypointsZ[path.current];
                            dx = tx - pos.x;
                            dz = tz - pos.z;
                            d2 = dist2(dx, dz);
                        }
                        else
                        {
                            // Path finished, steer to final target
                            path.valid = false;
                            tx = tgt.x;
                            tz = tgt.z;
                            dx = tx - pos.x;
                            dz = tz - pos.z;
                            d2 = dist2(dx, dz);
                        }
                    }

                    // If we just arrived at the final destination, stop and mark dirty
                    if (isFinal)
                    {
                        ecs.markDirty(m_velocityId, archetypeId, i);
                        ecs.markDirty(m_moveTargetId, archetypeId, i);
                        continue;
                    }
                }

                // Steer toward target
                if (d2 > 1e-8f)
                {
                    float dist = std::sqrt(d2);
                    float invDist = 1.0f / dist;
                    dx *= invDist;
                    dz *= invDist;

                    float speed = spd.value;

                    // Inertia / Smoothing: accelerate toward desired velocity
                    float targetVx = dx * speed;
                    float targetVz = dz * speed;

                    const float acceleration = 15.0f;

                    float diffX = targetVx - vel.x;
                    float diffZ = targetVz - vel.z;

                    vel.x += diffX * acceleration * dt;
                    vel.z += diffZ * acceleration * dt;
                    vel.y = 0.0f;

                    // Update Facing based on intended travel direction.
                    // This avoids jitter when LocalAvoidance perturbs velocity later in the frame.
                    facing.yaw = std::atan2(dx, dz);
                    ecs.markDirty(m_facingId, archetypeId, i);
                }

                ecs.markDirty(m_velocityId, archetypeId, i);

                // Keep active targets updating every frame
                if (tgt.active)
                    ecs.markDirty(m_moveTargetId, archetypeId, i);
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;
};
