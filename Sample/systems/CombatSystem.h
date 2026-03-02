#pragma once
/*
  CombatSystem.h
  --------------
  Auto-combat system for mass battles with variance mechanics.

  Per living entity each frame:
    1. Tick attack cooldown.
    2. Find nearest enemy (spatial grid + full-scan fallback).
    3. In melee range:
       a. Roll hit/miss (missChance).
       b. Roll crit (critChance -> critMultiplier).
       c. Roll damage in [damageMin, damageMax].
       d. Apply berserker rage bonus based on missing HP.
       e. Add cooldown jitter so attacks desync.
    4. Out of range -> charge toward enemy.
    5. HP <= 0 -> death anim, schedule removal.

  All tuning values are JSON-driven via BattleConfig.json "combat" section.
*/

#include "ECS/SystemFormat.h"
#include "ECS/Components.h"
#include "systems/SpatialIndexSystem.h"
#include "assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <iostream>

// Knight animation clip indices relevant to combat
namespace CombatAnims
{
    // Attacks: Stand_Attack_1 through Stand_Attack_8 (indices 36-43)
    constexpr uint32_t ATTACK_START = 36;
    constexpr uint32_t ATTACK_END = 43;

    // Damage reactions: Stand_Damage_0 through Stand_Damage_4 (indices 52-56)
    constexpr uint32_t DAMAGE_START = 52;
    constexpr uint32_t DAMAGE_END = 56;

    // Death: Stand_Death_0 through Stand_Death_3 (indices 61-64)
    constexpr uint32_t DEATH_START = 61;
    constexpr uint32_t DEATH_END = 64;

    // Run (for charging)
    constexpr uint32_t RUN = 28;

    // Idle
    constexpr uint32_t IDLE = 65;
}

class CombatSystem : public Engine::ECS::SystemBase
{
public:
    // Per-team aggregate stats for the HUD overlay
    struct TeamStats
    {
        int alive = 0;          // living units this frame
        int totalSpawned = 0;   // units that were initially spawned
        float currentHP = 0.0f; // sum of living units' health
        float maxHP = 0.0f;     // totalSpawned * maxHPPerUnit
    };

    // All combat tuning in one struct — maps 1:1 to BattleConfig.json "combat"
    struct CombatConfig
    {
        float meleeRange = 2.0f;
        float damageMin = 12.0f; // low end of damage roll
        float damageMax = 28.0f; // high end of damage roll
        float deathRemoveDelay = 3.0f;
        float maxHPPerUnit = 140.0f;

        float missChance = 0.20f;    // 0-1, chance an attack whiffs
        float critChance = 0.10f;    // 0-1, chance for a critical hit
        float critMultiplier = 2.0f; // damage multiplier on crit

        float rageMaxBonus = 0.50f;   // at 0 HP remaining, +50% damage
        float cooldownJitter = 0.30f; // ±30% random cooldown variation
        float staggerMax = 0.6f;      // max random initial cooldown offset
    };

    CombatSystem()
    {
        setRequiredNames({"Position", "Health", "Velocity", "MoveTarget", "MoveSpeed",
                          "Facing", "Team", "AttackCooldown", "RenderAnimation"});
        setExcludedNames({"Dead", "Disabled"});

        // Non-deterministic seed so each run plays differently
        std::random_device rd;
        m_rng.seed(rd());
    }

    const char *name() const override { return "CombatSystem"; }

    void setSpatialIndex(SpatialIndexSystem *spatial) { m_spatial = spatial; }
    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    // Apply full config (call after loading JSON)
    void applyConfig(const CombatConfig &cfg) { m_cfg = cfg; }
    const CombatConfig &config() const { return m_cfg; }

    // ── Battle start ──────────────────────────────────────────────
    //
    //  Click defines a point that MUST be on the path between both
    //  armies.  The charge works in two seamless legs:
    //
    //    Leg 1  Unit → Click point   (A* around obstacles)
    //    Leg 2  Click point → Enemy   (A* around obstacles)
    //
    //  No unit ever stops at the click point.  CombatSystem checks
    //  each unit's distance to the click; when within kPassRadius
    //  (3 m — well before SteeringSystem's 0.5 m arrival stop) it
    //  swaps MoveTarget to the enemy centroid and invalidates the
    //  path so PathfindingSystem replans the second leg.
    //
    //  The battle engagement point is NOT the click point — it
    //  emerges naturally from the relative path lengths and unit
    //  velocities as both armies run toward each other.
    //
    void startBattle(float clickX, float clickZ)
    {
        m_battleStarted = true;
        m_chargeActive = true;
        m_chargeIssued = false;
        m_battleClickX = clickX;
        m_battleClickZ = clickZ;
        std::cout << "[CombatSystem] Battle started! Click=(" << clickX << "," << clickZ << ")\n";
    }
    void startBattle() // legacy — no click target
    {
        m_battleStarted = true;
        m_chargeActive = false;
        m_chargeIssued = false;
        std::cout << "[CombatSystem] Battle started!\n";
    }
    bool isBattleStarted() const { return m_battleStarted; }

    // Human player control: one team requires spacebar to attack
    void setHumanTeam(int teamId) { m_humanTeamId = teamId; } // -1 = all AI
    void setHumanAttacking(bool attacking) { m_humanAttacking = attacking; }
    int humanTeamId() const { return m_humanTeamId; }
    bool isHumanAttacking() const { return m_humanAttacking; }

    // Legacy single-value setters still work
    void setMeleeRange(float range) { m_cfg.meleeRange = range; }
    void setDamagePerHit(float dmg)
    {
        m_cfg.damageMin = dmg * 0.6f;
        m_cfg.damageMax = dmg * 1.4f;
    }
    void setDeathRemoveDelay(float sec) { m_cfg.deathRemoveDelay = sec; }
    void setMaxHPPerUnit(float hp) { m_cfg.maxHPPerUnit = hp; }

    /// Returns latest team stats (updated every frame in update()).
    const TeamStats &getTeamStats(uint8_t teamId) const
    {
        static const TeamStats empty{};
        auto it = m_teamStats.find(teamId);
        return (it != m_teamStats.end()) ? it->second : empty;
    }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_positionId = registry.ensureId("Position");
        m_healthId = registry.ensureId("Health");
        m_velocityId = registry.ensureId("Velocity");
        m_moveTargetId = registry.ensureId("MoveTarget");
        m_teamId = registry.ensureId("Team");
        m_attackCooldownId = registry.ensureId("AttackCooldown");
        m_renderAnimId = registry.ensureId("RenderAnimation");
        m_facingId = registry.ensureId("Facing");
        m_deadId = registry.ensureId("Dead");
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (!m_spatial)
            return;

        // One-time startup: log config and stagger initial cooldowns
        if (!m_loggedStart)
        {
            std::cout << "[CombatSystem] Active. range=" << m_cfg.meleeRange
                      << " dmg=[" << m_cfg.damageMin << "," << m_cfg.damageMax << "]"
                      << " miss=" << (m_cfg.missChance * 100.0f) << "%"
                      << " crit=" << (m_cfg.critChance * 100.0f) << "%"
                      << " rage=" << (m_cfg.rageMaxBonus * 100.0f) << "%\n";
            staggerInitialCooldowns(ecs);
            m_loggedStart = true;
        }

        // Ensure query exists early so stats refresh works before battle
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        // ---- Phase 0: Refresh team stats (only when state changed) ----
        if (m_statsDirty)
        {
            refreshTeamStats(ecs);
            m_statsDirty = false;
        }

        // ---- Phase 1: Process pending death removals ----
        processDeathRemovals(ecs, dt);

        // If battle hasn't started yet, keep everyone idle
        if (!m_battleStarted)
            return;

        const auto &q = ecs.queries.get(m_queryId);

        // ── Charge: issue leg-1 targets (once) ──────────────────────
        if (m_chargeActive && !m_chargeIssued)
        {
            issueClickTargets(ecs, q);
            m_chargeIssued = true;
        }
        // ── Charge: promote units near click to leg-2 ───────────────
        if (m_chargeActive)
            promoteUnitsNearClick(ecs, q);

        const float meleeRange2 = m_cfg.meleeRange * m_cfg.meleeRange;

        // Clear per-frame deferred action buffers (reuse allocated memory — no heap alloc)
        m_damages.clear();
        m_attackAnims.clear();
        m_damageAnims.clear();
        m_moves.clear();
        m_stops.clear();

        // Local aliases so code below stays readable
        auto &damages = m_damages;
        auto &attackAnims = m_attackAnims;
        auto &damageAnims = m_damageAnims;
        auto &moves = m_moves;
        auto &stops = m_stops;

        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            if (!store.hasPosition() || !store.hasHealth() || !store.hasTeam() ||
                !store.hasAttackCooldown() || !store.hasVelocity() ||
                !store.hasMoveTarget() || !store.hasMoveSpeed() ||
                !store.hasRenderAnimation() || !store.hasFacing())
                continue;

            const auto &positions = store.positions();
            const auto &healths = store.healths();
            const auto &teams = store.teams();
            auto &cooldowns = store.attackCooldowns();
            const auto &moveSpeeds = store.moveSpeeds();
            const auto &anims = store.renderAnimations();
            const auto &entities = store.entities();

            const uint32_t n = store.size();

            for (uint32_t row = 0; row < n; ++row)
            {
                if (healths[row].value <= 0.0f)
                    continue; // skip already-dead units not yet migrated

                const Engine::ECS::Entity myEntity = entities[row];
                const uint8_t myTeam = teams[row].id;
                const float myX = positions[row].x;
                const float myZ = positions[row].z;

                // Tick cooldown (safe — only modifies this entity's own data)
                if (cooldowns[row].timer > 0.0f)
                    cooldowns[row].timer -= dt;

                // Find nearest living enemy via spatial grid
                float bestDist2 = 1e18f;
                float bestEX = myX;
                float bestEZ = myZ;
                Engine::ECS::Entity bestEnemy{};

                m_spatial->forNeighbors(myX, myZ, [&](uint32_t neighborStoreId, uint32_t neighborRow)
                                        {
                    auto *ns = ecs.stores.get(neighborStoreId);
                    if (!ns || !ns->hasPosition() || !ns->hasHealth() || !ns->hasTeam())
                        return;
                    if (neighborRow >= ns->size())
                        return;
                    if (neighborStoreId == archetypeId && neighborRow == row)
                        return;

                    if (ns->teams()[neighborRow].id == myTeam)
                        return;
                    if (ns->healths()[neighborRow].value <= 0.0f)
                        return;

                    const float ex = ns->positions()[neighborRow].x;
                    const float ez = ns->positions()[neighborRow].z;
                    const float d2 = (ex - myX) * (ex - myX) + (ez - myZ) * (ez - myZ);
                    const Engine::ECS::Entity cand = ns->entities()[neighborRow];
                    const float tieEps = 1e-6f;
                    const bool better = (d2 < bestDist2 - tieEps) ||
                                        (std::fabs(d2 - bestDist2) <= tieEps && (!bestEnemy.valid() || cand.index < bestEnemy.index));
                    if (better)
                    {
                        bestDist2 = d2;
                        bestEX = ex;
                        bestEZ = ez;
                        bestEnemy = cand;
                    } });

                // Fallback: full scan when spatial grid finds nothing
                if (!bestEnemy.valid())
                {
                    for (uint32_t otherArchId : q.matchingArchetypeIds)
                    {
                        auto *os = ecs.stores.get(otherArchId);
                        if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                            continue;

                        const auto &oPos = os->positions();
                        const auto &oHP = os->healths();
                        const auto &oTeam = os->teams();
                        const auto &oEnt = os->entities();
                        const uint32_t oN = os->size();

                        for (uint32_t oRow = 0; oRow < oN; ++oRow)
                        {
                            if (otherArchId == archetypeId && oRow == row)
                                continue;
                            if (oTeam[oRow].id == myTeam)
                                continue;
                            if (oHP[oRow].value <= 0.0f)
                                continue;

                            const float d2 = (oPos[oRow].x - myX) * (oPos[oRow].x - myX) +
                                             (oPos[oRow].z - myZ) * (oPos[oRow].z - myZ);
                            const Engine::ECS::Entity cand = oEnt[oRow];
                            const float tieEps = 1e-6f;
                            const bool better = (d2 < bestDist2 - tieEps) ||
                                                (std::fabs(d2 - bestDist2) <= tieEps && (!bestEnemy.valid() || cand.index < bestEnemy.index));
                            if (better)
                            {
                                bestDist2 = d2;
                                bestEX = oPos[oRow].x;
                                bestEZ = oPos[oRow].z;
                                bestEnemy = cand;
                            }
                        }
                    }
                }

                if (!bestEnemy.valid())
                {
                    // No enemy found — during charge keep running,
                    // otherwise stop.
                    if (!m_chargeActive)
                    {
                        float yaw = store.facings()[row].yaw;
                        const bool clearVel = (store.moveTargets()[row].active != 0);
                        stops.push_back({myEntity, yaw, clearVel});
                    }
                    continue;
                }

                // Compute facing toward enemy
                const float dx = bestEX - myX;
                const float dz = bestEZ - myZ;
                const float yaw = (dx * dx + dz * dz > 1e-6f)
                                      ? std::atan2(dx, dz)
                                      : store.facings()[row].yaw;

                // Squared-distance comparison avoids sqrt per entity per frame
                if (bestDist2 <= meleeRange2)
                {
                    // First melee contact ends the charge phase.
                    if (m_chargeActive)
                        m_chargeActive = false;

                    // In melee range — stop and attack
                    const bool clearVel = (store.moveTargets()[row].active != 0);
                    stops.push_back({myEntity, yaw, clearVel});

                    if (cooldowns[row].timer <= 0.0f)
                    {
                        // Reset cooldown with jitter: interval * (1 ± jitter)
                        float jitter = 1.0f + m_realDist(m_rng) * m_cfg.cooldownJitter;
                        cooldowns[row].timer = cooldowns[row].interval * jitter;

                        // Always play attack animation
                        uint32_t attackClip = CombatAnims::ATTACK_START +
                                              (m_rng() % (CombatAnims::ATTACK_END - CombatAnims::ATTACK_START + 1));
                        attackAnims.push_back({myEntity, attackClip, 1.5f, false});

                        // --- Roll hit / miss ---
                        if (m_unitDist(m_rng) < m_cfg.missChance)
                        {
                            // Miss! No damage, no damage anim on enemy
                            // (attacker still plays swing anim — just whiffs)
                        }
                        else
                        {
                            // --- Roll damage in [damageMin, damageMax] ---
                            float baseDmg = m_cfg.damageMin +
                                            m_unitDist(m_rng) * (m_cfg.damageMax - m_cfg.damageMin);

                            // --- Berserker rage: bonus damage based on missing HP ---
                            float myHPFrac = healths[row].value / m_cfg.maxHPPerUnit;
                            float rageMult = 1.0f + m_cfg.rageMaxBonus * (1.0f - std::clamp(myHPFrac, 0.0f, 1.0f));
                            baseDmg *= rageMult;

                            // --- Roll critical hit ---
                            bool isCrit = (m_unitDist(m_rng) < m_cfg.critChance);
                            if (isCrit)
                                baseDmg *= m_cfg.critMultiplier;

                            // Queue damage on enemy
                            damages.push_back({bestEnemy, baseDmg});

                            // Queue damage anim on enemy
                            uint32_t dmgClip = CombatAnims::DAMAGE_START +
                                               (m_rng() % (CombatAnims::DAMAGE_END - CombatAnims::DAMAGE_START + 1));
                            // Crits play damage anim faster for visual punch
                            float dmgAnimSpeed = isCrit ? 1.4f : 1.0f;
                            damageAnims.push_back({bestEnemy, dmgClip, dmgAnimSpeed, false});
                        }
                    }
                }
                else
                {
                    // Out of range — chase nearest enemy.
                    // During charge, only skip units still on leg 1
                    // (their MoveTarget equals the click point).
                    // Promoted units (past click) chase normally.
                    bool skipChase = false;
                    if (m_chargeActive)
                    {
                        const auto &tgt = store.moveTargets()[row];
                        float tdx = tgt.x - m_battleClickX;
                        float tdz = tgt.z - m_battleClickZ;
                        skipChase = tgt.active && (tdx * tdx + tdz * tdz < 1.0f);
                    }
                    if (!skipChase)
                    {
                        moves.push_back({myEntity,
                                         bestEX, bestEZ, true, yaw,
                                         CombatAnims::RUN,
                                         anims[row].clipIndex != CombatAnims::RUN});
                    }
                }
            }
        }

        // ---- Phase 3: Apply deferred actions (safe to mutate stores now) ----

        // Apply stops
        for (const auto &s : stops)
        {
            auto *rec = ecs.entities.find(s.entity);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size())
                continue;

            if (s.clearVelocity && st->hasVelocity())
                st->velocities()[rec->row] = {0.0f, 0.0f, 0.0f};
            if (st->hasMoveTarget())
                st->moveTargets()[rec->row].active = 0;
            if (st->hasFacing())
                st->facings()[rec->row].yaw = s.yaw;

            if (st->hasMoveTarget())
                ecs.markDirty(m_moveTargetId, rec->archetypeId, rec->row);
            if (s.clearVelocity)
                ecs.markDirty(m_velocityId, rec->archetypeId, rec->row);
        }

        // Apply moves (set MoveTarget — PathfindingSystem + SteeringSystem handle velocity)
        for (const auto &mv : moves)
        {
            auto *rec = ecs.entities.find(mv.entity);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size())
                continue;

            if (st->hasMoveTarget())
            {
                auto &tgt = st->moveTargets()[rec->row];
                // Only re-path if target changed significantly (avoid thrashing)
                float dtx = tgt.x - mv.tx;
                float dtz = tgt.z - mv.tz;
                bool targetMoved = (dtx * dtx + dtz * dtz > 4.0f) || !tgt.active;
                if (targetMoved)
                {
                    tgt.x = mv.tx;
                    tgt.y = 0.0f;
                    tgt.z = mv.tz;
                    tgt.active = mv.active ? 1 : 0;

                    // Invalidate existing path so PathfindingSystem replans
                    // A* to the NEW target instead of following the old route.
                    if (st->hasPath())
                        st->paths()[rec->row].valid = false;

                    ecs.markDirty(m_moveTargetId, rec->archetypeId, rec->row);
                }
            }
            if (st->hasFacing())
                st->facings()[rec->row].yaw = mv.yaw;
            if (mv.setRunAnim && st->hasRenderAnimation())
            {
                st->renderAnimations()[rec->row].clipIndex = mv.runClip;
                st->renderAnimations()[rec->row].timeSec = 0.0f;
                st->renderAnimations()[rec->row].playing = true;
                st->renderAnimations()[rec->row].loop = true;
                st->renderAnimations()[rec->row].speed = 1.0f;
                ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
            }
        }

        // Apply attack anims
        for (const auto &aa : attackAnims)
        {
            auto *rec = ecs.entities.find(aa.entity);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size() || !st->hasRenderAnimation())
                continue;

            st->renderAnimations()[rec->row].clipIndex = aa.clipIndex;
            st->renderAnimations()[rec->row].timeSec = 0.0f;
            st->renderAnimations()[rec->row].playing = true;
            st->renderAnimations()[rec->row].loop = aa.loop;
            st->renderAnimations()[rec->row].speed = aa.speed;
            ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
        }

        // Apply damage
        for (const auto &d : damages)
        {
            auto *rec = ecs.entities.find(d.target);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size() || !st->hasHealth())
                continue;

            st->healths()[rec->row].value -= d.damage;
            m_statsDirty = true;
        }

        // Apply damage anims (only if still alive — death anim will override below)
        for (const auto &da : damageAnims)
        {
            auto *rec = ecs.entities.find(da.entity);
            if (!rec)
                continue;
            auto *st = ecs.stores.get(rec->archetypeId);
            if (!st || rec->row >= st->size())
                continue;
            if (!st->hasHealth() || !st->hasRenderAnimation())
                continue;

            // Only play damage anim if still alive
            if (st->healths()[rec->row].value > 0.0f)
            {
                st->renderAnimations()[rec->row].clipIndex = da.clipIndex;
                st->renderAnimations()[rec->row].timeSec = 0.0f;
                st->renderAnimations()[rec->row].playing = true;
                st->renderAnimations()[rec->row].loop = false;
                st->renderAnimations()[rec->row].speed = da.speed;
                ecs.markDirty(m_renderAnimId, rec->archetypeId, rec->row);
            }
        }

        // ---- Phase 4: Handle newly dead entities (deferred tag migration) ----
        // Check all matching stores for entities with health <= 0 that aren't in death queue yet
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr || !storePtr->hasHealth())
                continue;

            const auto &hp = storePtr->healths();
            const auto &ent = storePtr->entities();
            const uint32_t n = storePtr->size();

            // Collect dead entities first (iterating while collecting, no mutations)
            m_newlyDead.clear();
            for (uint32_t row = 0; row < n; ++row)
            {
                if (hp[row].value <= 0.0f)
                {
                    Engine::ECS::Entity e = ent[row];
                    // O(1) set lookup instead of linear scan of death queue
                    if (m_deathQueueSet.count(e.index) == 0)
                        m_newlyDead.push_back(e);
                }
            }

            // Now apply death effects — these migrate entities so must be done outside iteration
            for (const auto &deadEntity : m_newlyDead)
            {
                auto *rec = ecs.entities.find(deadEntity);
                if (!rec)
                    continue;
                auto *st = ecs.stores.get(rec->archetypeId);
                if (!st || rec->row >= st->size())
                    continue;

                // Play death animation
                if (st->hasRenderAnimation())
                {
                    uint32_t deathClip = CombatAnims::DEATH_START +
                                         (m_rng() % (CombatAnims::DEATH_END - CombatAnims::DEATH_START + 1));
                    st->renderAnimations()[rec->row].clipIndex = deathClip;
                    st->renderAnimations()[rec->row].timeSec = 0.0f;
                    st->renderAnimations()[rec->row].playing = true;
                    st->renderAnimations()[rec->row].loop = false;
                    st->renderAnimations()[rec->row].speed = 1.0f;
                }

                // Stop moving
                if (st->hasVelocity())
                    st->velocities()[rec->row] = {0.0f, 0.0f, 0.0f};
                if (st->hasMoveTarget())
                    st->moveTargets()[rec->row].active = 0;

                // Schedule removal and migrate to Dead archetype
                m_deathQueue.push_back({deadEntity, m_cfg.deathRemoveDelay});
                m_deathQueueSet.insert(deadEntity.index);
                m_statsDirty = true;
                ecs.addTag(deadEntity, m_deadId);
            }
        }
    }

private:
    void refreshTeamStats(Engine::ECS::ECSContext &ecs)
    {
        // Reset counts
        for (auto &[id, s] : m_teamStats)
        {
            s.alive = 0;
            s.currentHP = 0.0f;
        }

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            return;

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasHealth() || !st->hasTeam())
                continue;
            const auto &hp = st->healths();
            const auto &teams = st->teams();
            const uint32_t n = st->size();
            for (uint32_t row = 0; row < n; ++row)
            {
                uint8_t tid = teams[row].id;
                auto &ts = m_teamStats[tid];
                // First time we see this team, record max HP
                if (ts.totalSpawned == 0)
                    ts.maxHP = 0.0f;
                ts.alive++;
                ts.currentHP += std::max(0.0f, hp[row].value);
            }
        }

        // Also count dead entities in death queue per team (they were spawned but are dying)
        // Update totalSpawned = alive + dead-in-queue + already-removed
        // Simplification: track totalSpawned incrementally on first non-zero alive frame
        for (auto &[id, s] : m_teamStats)
        {
            if (s.alive > s.totalSpawned)
                s.totalSpawned = s.alive;
            s.maxHP = s.totalSpawned * m_cfg.maxHPPerUnit;
        }
    }

    void processDeathRemovals(Engine::ECS::ECSContext &ecs, float dt)
    {
        // Swap-and-pop instead of erase — O(1) per removal instead of O(N)
        size_t i = 0;
        while (i < m_deathQueue.size())
        {
            m_deathQueue[i].timeRemaining -= dt;
            if (m_deathQueue[i].timeRemaining <= 0.0f)
            {
                auto &pd = m_deathQueue[i];
                // Destroy the entity
                if (ecs.entities.isAlive(pd.entity))
                {
                    auto *rec = ecs.entities.find(pd.entity);
                    if (rec)
                    {
                        auto *store = ecs.stores.get(rec->archetypeId);
                        if (store)
                        {
                            Engine::ECS::Entity moved = store->destroyRowSwap(rec->row);
                            if (moved.valid())
                                ecs.entities.attach(moved, rec->archetypeId, rec->row);
                        }
                    }
                    ecs.entities.destroy(pd.entity);
                }
                m_deathQueueSet.erase(pd.entity.index);
                // Swap with last and pop — don't increment i
                pd = m_deathQueue.back();
                m_deathQueue.pop_back();
            }
            else
            {
                ++i;
            }
        }
    }

    struct PendingDeath
    {
        Engine::ECS::Entity entity;
        float timeRemaining;
    };

    // Deferred-action structs (defined once at class level, not per-frame)
    struct DamageAction
    {
        Engine::ECS::Entity target;
        float damage;
    };
    struct AnimAction
    {
        Engine::ECS::Entity entity;
        uint32_t clipIndex;
        float speed;
        bool loop;
    };
    struct MoveAction
    {
        Engine::ECS::Entity entity;
        float tx, tz;
        bool active;
        float yaw;
        uint32_t runClip;
        bool setRunAnim;
    };
    struct StopAction
    {
        Engine::ECS::Entity entity;
        float yaw;
        bool clearVelocity;
    };

    // ── Charge helpers ─────────────────────────────────────────────

    // Radius at which a unit is "passing through" the click point.
    // Must be > SteeringSystem arrivalRadius (0.5 m) so the redirect
    // happens BEFORE SteeringSystem stops the unit.
    static constexpr float kPassRadius = 3.0f;
    static constexpr float kPassRadius2 = kPassRadius * kPassRadius;

    // Leg 1: set every unit's MoveTarget to the click point.
    // PathfindingSystem will A* around obstacles on the next frame.
    void issueClickTargets(Engine::ECS::ECSContext &ecs,
                           const Engine::ECS::Query &q)
    {
        for (uint32_t aid : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(aid);
            if (!st || !st->hasMoveTarget() || !st->hasHealth())
                continue;
            auto &mt = st->moveTargets();
            const auto &hp = st->healths();
            for (uint32_t r = 0; r < st->size(); ++r)
            {
                if (hp[r].value <= 0.0f)
                    continue;
                mt[r].x = m_battleClickX;
                mt[r].y = 0.0f;
                mt[r].z = m_battleClickZ;
                mt[r].active = 1;
                ecs.markDirty(m_moveTargetId, aid, r);
            }
        }
        std::cout << "[CombatSystem] Leg-1: all units → click point ("
                  << m_battleClickX << "," << m_battleClickZ << ")\n";
    }

    // Per-frame: any unit whose position is within kPassRadius of
    // the click point gets promoted to leg 2.  We find the nearest
    // LIVE enemy via spatial index and set MoveTarget there.
    // The main combat loop then keeps chasing per-frame from here.
    // Because kPassRadius (3 m) > SteeringSystem arrival (0.5 m),
    // the redirect happens while the unit is still RUNNING — no stop.
    void promoteUnitsNearClick(Engine::ECS::ECSContext &ecs,
                               const Engine::ECS::Query &q)
    {
        for (uint32_t aid : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(aid);
            if (!st || !st->hasMoveTarget() || !st->hasHealth() ||
                !st->hasPosition() || !st->hasTeam())
                continue;

            auto &mt = st->moveTargets();
            const auto &pos = st->positions();
            const auto &hp = st->healths();
            const auto &tm = st->teams();
            const bool hasP = st->hasPath();

            for (uint32_t r = 0; r < st->size(); ++r)
            {
                if (hp[r].value <= 0.0f)
                    continue;
                if (!mt[r].active)
                    continue;

                // Already promoted?  (MoveTarget no longer equals click)
                float dtx = mt[r].x - m_battleClickX;
                float dtz = mt[r].z - m_battleClickZ;
                if (dtx * dtx + dtz * dtz > 1.0f)
                    continue;

                // Close enough to click → promote to leg 2
                float dx = pos[r].x - m_battleClickX;
                float dz = pos[r].z - m_battleClickZ;
                if (dx * dx + dz * dz > kPassRadius2)
                    continue;

                // Find nearest LIVE enemy via spatial index (O(1) vs O(N))
                uint8_t myTeam = tm[r].id;
                float bestEX = pos[r].x, bestEZ = pos[r].z;
                float bestD2 = 1e18f;

                if (m_spatial)
                {
                    // Try spatial grid first — fast local lookup
                    m_spatial->forNeighbors(pos[r].x, pos[r].z,
                                            [&](uint32_t nStoreId, uint32_t nRow)
                                            {
                                                auto *ns = ecs.stores.get(nStoreId);
                                                if (!ns || !ns->hasPosition() || !ns->hasHealth() || !ns->hasTeam())
                                                    return;
                                                if (nRow >= ns->size())
                                                    return;
                                                if (ns->teams()[nRow].id == myTeam)
                                                    return;
                                                if (ns->healths()[nRow].value <= 0.0f)
                                                    return;
                                                float ex = ns->positions()[nRow].x;
                                                float ez = ns->positions()[nRow].z;
                                                float d2 = (ex - pos[r].x) * (ex - pos[r].x) + (ez - pos[r].z) * (ez - pos[r].z);
                                                if (d2 < bestD2)
                                                {
                                                    bestD2 = d2;
                                                    bestEX = ex;
                                                    bestEZ = ez;
                                                }
                                            });
                }

                // Fallback: full scan if spatial found nothing
                if (bestD2 > 1e17f)
                {
                    for (uint32_t oaid : q.matchingArchetypeIds)
                    {
                        auto *os = ecs.stores.get(oaid);
                        if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                            continue;
                        const auto &op = os->positions();
                        const auto &oh = os->healths();
                        const auto &ot = os->teams();
                        for (uint32_t orow = 0; orow < os->size(); ++orow)
                        {
                            if (ot[orow].id == myTeam)
                                continue;
                            if (oh[orow].value <= 0.0f)
                                continue;
                            float d2 = (op[orow].x - pos[r].x) * (op[orow].x - pos[r].x) + (op[orow].z - pos[r].z) * (op[orow].z - pos[r].z);
                            if (d2 < bestD2)
                            {
                                bestD2 = d2;
                                bestEX = op[orow].x;
                                bestEZ = op[orow].z;
                            }
                        }
                    }
                }

                mt[r].x = bestEX;
                mt[r].y = 0.0f;
                mt[r].z = bestEZ;
                mt[r].active = 1;

                // Invalidate current path so PathfindingSystem A*-replans
                if (hasP)
                    st->paths()[r].valid = false;

                ecs.markDirty(m_moveTargetId, aid, r);
            }
        }
    }

    // Stagger initial cooldowns so units don't all attack on the same frame
    void staggerInitialCooldowns(Engine::ECS::ECSContext &ecs)
    {
        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            return;
        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasAttackCooldown())
                continue;
            auto &cds = st->attackCooldowns();
            const uint32_t n = st->size();
            for (uint32_t row = 0; row < n; ++row)
                cds[row].timer = m_unitDist(m_rng) * m_cfg.staggerMax;
        }
    }

    SpatialIndexSystem *m_spatial = nullptr;
    Engine::AssetManager *m_assets = nullptr;
    bool m_loggedStart = false;
    bool m_battleStarted = false; // gated by ground click
    bool m_statsDirty = true;     // refresh team stats only when needed

    // Charge state
    float m_battleClickX = 0.0f;
    float m_battleClickZ = 0.0f;
    bool m_chargeActive = false; // true while armies are charging
    bool m_chargeIssued = false; // leg-1 targets sent

    // All combat tuning lives here
    CombatConfig m_cfg;

    // Human player control
    int m_humanTeamId = -1;        // -1 = all AI, 0 = team A is human, 1 = team B
    bool m_humanAttacking = false; // true while spacebar is held

    // Per-team stats (refreshed each frame)
    std::unordered_map<uint8_t, TeamStats> m_teamStats;

    // Component IDs
    uint32_t m_positionId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_healthId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_velocityId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_teamId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_attackCooldownId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_facingId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_deadId = Engine::ECS::ComponentRegistry::InvalidID;

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;

    std::vector<PendingDeath> m_deathQueue;
    std::unordered_set<uint32_t> m_deathQueueSet; // O(1) death-queue membership test

    // Persistent per-frame buffers (cleared each frame, memory reused)
    std::vector<DamageAction> m_damages;
    std::vector<AnimAction> m_attackAnims;
    std::vector<AnimAction> m_damageAnims;
    std::vector<MoveAction> m_moves;
    std::vector<StopAction> m_stops;
    std::vector<Engine::ECS::Entity> m_newlyDead;

    std::mt19937 m_rng;                                            // seeded non-deterministically in constructor
    std::uniform_real_distribution<float> m_unitDist{0.0f, 1.0f};  // [0,1)
    std::uniform_real_distribution<float> m_realDist{-1.0f, 1.0f}; // [-1,1)
};
