#pragma once
/*
  LocalAvoidanceSystem.h
  ----------------------
  Purpose:
    - Adjust velocities to prevent overlap using local separation, based on neighbors
      found via SpatialIndexSystem (hash grid, cellSize = neighbor radius R).

  Requirements:
    - Components present in stores: "Position", "Velocity", "Radius", "AvoidanceParams".
        - Optional component: "Separation" (extra desired spacing beyond radii).
    - SpatialIndexSystem must have run earlier in the frame (grid built).
    - Steering should have already produced a "preferred" velocity, stored in Velocity.
      This system keeps final speeds close to that magnitude.

  Suggested order per frame:
    CommandSystem -> SteeringSystem -> SpatialIndexSystem -> LocalAvoidanceSystem -> MovementSystem
*/

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include "ECS/ArchetypeStore.h"

// The grid index system for neighbor queries
#include "systems/SpatialIndexSystem.h"

#include <algorithm>
#include <cstdint>
#include <cmath>

class LocalAvoidanceSystem : public Engine::ECS::SystemBase
{
public:
    LocalAvoidanceSystem(const SpatialIndexSystem *grid = nullptr)
        : m_grid(grid)
    {
        // Require the data we adjust/read
        setRequiredNames({"Position", "Velocity", "Radius", "AvoidanceParams"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "LocalAvoidanceSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_velocityId = registry.ensureId("Velocity");
        m_moveTargetId = registry.ensureId("MoveTarget");
    }

    void setGrid(const SpatialIndexSystem *grid) { m_grid = grid; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (!m_grid)
            return;
        if (dt <= 0.0f)
            return;

        auto clamp = [](float v, float a, float b)
        { return std::max(a, std::min(v, b)); };

        auto dot2 = [](float ax, float az, float bx, float bz)
        { return ax * bx + az * bz; };

        auto safeNormalize = [](float &x, float &z)
        {
            const float l2 = x * x + z * z;
            if (l2 <= 1e-12f)
            {
                x = 1.0f;
                z = 0.0f;
                return;
            }
            const float inv = 1.0f / std::sqrt(l2);
            x *= inv;
            z *= inv;
        };

        auto lerp = [](float a, float b, float t)
        { return a + (b - a) * t; };

        auto hashAngle = [](uint32_t v) -> float
        {
            v ^= v >> 16;
            v *= 0x7feb352du;
            v ^= v >> 15;
            v *= 0x846ca68bu;
            v ^= v >> 16;
            const float u = (v & 0xFFFFFFu) / float(0x1000000u);
            return u * 6.28318530718f;
        };

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
        {
            // Wake avoidance when movement happens (Position/Velocity), and also when targets change.
            Engine::ECS::ComponentMask dirty;
            dirty.set(m_positionId);
            dirty.set(m_velocityId);
            dirty.set(m_moveTargetId);
            m_queryId = ecs.queries.createDirtyQuery(required(), excluded(), dirty, ecs.stores);
        }

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            auto dirtyRows = ecs.queries.consumeDirtyRows(m_queryId, archetypeId);
            if (dirtyRows.empty())
                continue;

            auto &positions = store.positions();
            auto &velocities = store.velocities();
            auto &radii = store.radii();
            auto &params = store.avoidanceParams();
            const bool hasSep = store.hasSeparation();
            const auto *sepsPtr = hasSep ? &store.separations() : nullptr;
            const bool hasMoveTarget = store.hasMoveTarget();
            const auto *targetsPtr = hasMoveTarget ? &store.moveTargets() : nullptr;
            const bool hasTeam = store.hasTeam();
            const auto *teamsPtr = hasTeam ? &store.teams() : nullptr;
            const auto &ents = store.entities();
            const uint32_t n = store.size();

            for (uint32_t row : dirtyRows)
            {
                if (row >= n)
                    continue;

                auto &p = positions[row];
                auto &v = velocities[row];
                const auto &r = radii[row];
                const auto &ap = params[row];
                const float sepSelf = sepsPtr ? (*sepsPtr)[row].value : 0.0f;

                const float vPrefX = v.x;
                const float vPrefZ = v.z;
                const float prefSpeed2 = vPrefX * vPrefX + vPrefZ * vPrefZ;
                const float prefSpeed = (prefSpeed2 > 1e-12f) ? std::sqrt(prefSpeed2) : 0.0f;

                const float horizon = std::max(0.0f, ap.predictionTime);
                const float nearGoalRadius = std::max(ap.nearGoalRadius, 1e-3f);
                const float interactSlack = ap.interactSlack;
                const float falloffWeight = ap.falloffWeight;
                const float predictiveWeight = ap.predictiveWeight;
                const float pressureBoost = std::max(0.0f, ap.pressureBoost);
                const float maxStopSpeed = std::max(0.0f, ap.maxStopSpeed);

                float arrivalBoost = 1.0f;
                if (targetsPtr && (*targetsPtr)[row].active)
                {
                    const auto &tgt = (*targetsPtr)[row];
                    const float dx = tgt.x - p.x;
                    const float dz = tgt.z - p.z;
                    const float dist = std::sqrt(dx * dx + dz * dz);
                    const float t = clamp(1.0f - (dist / nearGoalRadius), 0.0f, 1.0f);
                    arrivalBoost = 1.0f + std::max(0.0f, ap.nearGoalBoost) * t;
                }
                else if (prefSpeed <= 1e-4f)
                {
                    arrivalBoost = std::max(0.0f, ap.stoppedBoost);
                }

                const uint8_t myTeam = (teamsPtr ? (*teamsPtr)[row].id : 0u);

                float accDirX = 0.0f;
                float accDirZ = 0.0f;
                bool hasPressure = false;

                m_grid->forNeighbors(p.x, p.z, [&](uint32_t nStoreId, uint32_t nRow)
                                     {
                    if (nStoreId == archetypeId && nRow == row) return;

                    const auto* nStore = ecs.stores.get(nStoreId);
                    if (!nStore) return;
                    if (!nStore->hasPosition() || !nStore->hasRadius()) return;
                    if (nRow >= nStore->size()) return;

                    const auto& np = nStore->positions()[nRow];
                    const auto& nr = nStore->radii()[nRow];

                    const bool nHasSep = nStore->hasSeparation();
                    const bool nHasTeam = nStore->hasTeam();
                    const bool nHasVel = nStore->hasVelocity();

                    float desiredSep = 0.0f;
                    if (!teamsPtr || !nHasTeam)
                    {
                        desiredSep = sepSelf + (nHasSep ? nStore->separations()[nRow].value : 0.0f);
                    }
                    else
                    {
                        const uint8_t otherTeam = nStore->teams()[nRow].id;
                        if (otherTeam == myTeam)
                            desiredSep = sepSelf + (nHasSep ? nStore->separations()[nRow].value : 0.0f);
                        else
                            desiredSep = 0.0f;
                    }

                    const float desiredDist = (r.r + nr.r) + desiredSep;
                    const float interactDist = desiredDist + interactSlack;

                    float dx = p.x - np.x;
                    float dz = p.z - np.z;
                    const float dist2 = dx * dx + dz * dz;
                    if (dist2 > interactDist * interactDist)
                        return;

                    float dist = (dist2 > 1e-12f) ? std::sqrt(dist2) : 0.0f;
                    float awayX = dx;
                    float awayZ = dz;
                    if (dist <= 1e-6f)
                    {
                        const float a = hashAngle(static_cast<uint32_t>(ents[row].index) ^ (nStoreId * 16777619u + nRow));
                        awayX = std::cos(a);
                        awayZ = std::sin(a);
                        dist = 0.0f;
                    }
                    else
                    {
                        awayX /= dist;
                        awayZ /= dist;
                    }

                    const float fall = clamp(1.0f - (dist / std::max(interactDist, 1e-4f)), 0.0f, 1.0f);
                    const float penetration = desiredDist - dist;
                    if (penetration > 0.0f)
                    {
                        const float w = clamp(penetration / std::max(desiredDist, 1e-4f), 0.0f, 1.0f);
                        accDirX += awayX * w;
                        accDirZ += awayZ * w;
                        hasPressure = true;
                    }
                    else
                    {
                        const float w = std::max(0.0f, falloffWeight) * fall;
                        if (w > 0.0f)
                        {
                            accDirX += awayX * w;
                            accDirZ += awayZ * w;
                        }
                    }

                    float nVx = 0.0f;
                    float nVz = 0.0f;
                    if (nHasVel)
                    {
                        const auto &nv = nStore->velocities()[nRow];
                        nVx = nv.x;
                        nVz = nv.z;
                    }

                    const float relX = p.x - np.x;
                    const float relZ = p.z - np.z;
                    const float rvX = vPrefX - nVx;
                    const float rvZ = vPrefZ - nVz;
                    const float rv2 = rvX * rvX + rvZ * rvZ;
                    if (horizon > 1e-6f && rv2 > 1e-8f)
                    {
                        float ttc = -dot2(relX, relZ, rvX, rvZ) / rv2;
                        ttc = clamp(ttc, 0.0f, horizon);

                        const float cx = relX + rvX * ttc;
                        const float cz = relZ + rvZ * ttc;
                        const float cd2 = cx * cx + cz * cz;
                        if (cd2 < desiredDist * desiredDist)
                        {
                            float dirX = cx;
                            float dirZ = cz;
                            safeNormalize(dirX, dirZ);

                            const float cd = std::sqrt(std::max(cd2, 1e-12f));
                            const float w = clamp((desiredDist - cd) / std::max(desiredDist, 1e-4f), 0.0f, 1.0f) * (1.0f - (ttc / horizon));
                            accDirX += dirX * (std::max(0.0f, predictiveWeight) * w);
                            accDirZ += dirZ * (std::max(0.0f, predictiveWeight) * w);
                            hasPressure = true;
                        }
                    } });

                if (!hasPressure && (std::fabs(accDirX) + std::fabs(accDirZ) < 1e-6f))
                {
                    const bool hasActiveTarget = (targetsPtr && (*targetsPtr)[row].active);
                    if (!hasActiveTarget)
                    {
                        v.x = 0.0f;
                        v.z = 0.0f;
                        ecs.markDirty(m_velocityId, archetypeId, row);
                    }
                    continue;
                }

                // If forces cancel out (rare but possible in symmetric crowds), pick a stable per-entity
                // direction so overlapped stacks can still resolve without introducing frame-to-frame noise.
                const float accL2 = accDirX * accDirX + accDirZ * accDirZ;
                if (accL2 <= 1e-12f)
                {
                    const float a = hashAngle(static_cast<uint32_t>(ents[row].index));
                    accDirX = std::cos(a);
                    accDirZ = std::sin(a);
                }
                else
                {
                    safeNormalize(accDirX, accDirZ);
                }

                float accelMag = ap.strength * arrivalBoost;
                if (hasPressure)
                    accelMag *= pressureBoost;

                float aX = accDirX * accelMag;
                float aZ = accDirZ * accelMag;

                const float aMax = std::max(0.0f, ap.maxAccel) * arrivalBoost;
                const float aLen2 = aX * aX + aZ * aZ;
                if (aMax > 0.0f && aLen2 > aMax * aMax)
                {
                    const float aLen = std::sqrt(aLen2);
                    const float s = aMax / std::max(aLen, 1e-6f);
                    aX *= s;
                    aZ *= s;
                }

                float vNewX = vPrefX + aX * dt;
                float vNewZ = vPrefZ + aZ * dt;

                const float newSpeed2 = vNewX * vNewX + vNewZ * vNewZ;
                const float newSpeed = (newSpeed2 > 1e-12f) ? std::sqrt(newSpeed2) : 0.0f;
                if (prefSpeed > 1e-4f)
                {
                    if (newSpeed > prefSpeed && newSpeed > 1e-6f)
                    {
                        const float s = prefSpeed / newSpeed;
                        vNewX *= s;
                        vNewZ *= s;
                    }
                }
                else
                {
                    if (newSpeed > maxStopSpeed && newSpeed > 1e-6f)
                    {
                        const float s = maxStopSpeed / newSpeed;
                        vNewX *= s;
                        vNewZ *= s;
                    }
                }

                const float t = clamp(ap.blend, 0.0f, 1.0f);
                v.x = lerp(vPrefX, vNewX, t);
                v.z = lerp(vPrefZ, vNewZ, t);

                // Keep row active while avoidance is producing corrections.
                ecs.markDirty(m_velocityId, archetypeId, row);
            }
        }
    }

private:
    const SpatialIndexSystem *m_grid = nullptr; // not owned
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
};