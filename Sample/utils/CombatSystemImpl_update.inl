#pragma once

// Main update loop for CombatSystem.
// Included by Sample/utils/CombatSystemImpl.inl.

#include "utils/JobSystem.h"

inline void CombatSystem::update(Engine::ECS::ECSContext &ecs, float dt)
{
    if (!m_spatial)
        return;

    ++m_frameCounter;

    auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
    {
        return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
    };

    // One-time startup: log config and stagger initial cooldowns
    if (!m_loggedStart)
    {
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        std::cout << "[CombatSystem] Active. range=" << m_cfg.meleeRange
                  << " dmg=[" << m_cfg.damageMin << "," << m_cfg.damageMax << "]"
                  << " miss=" << (m_cfg.missChance * 100.0f) << "%"
                  << " crit=" << (m_cfg.critChance * 100.0f) << "%"
                  << " rage=" << (m_cfg.rageMaxBonus * 100.0f) << "%\n";
#endif
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

    // If battle hasn't started yet, auto-start when armies get close enough.
    if (!m_battleStarted)
    {
        const float engageR = std::max(0.0f, m_cfg.engageRange);
        if (engageR <= 0.0f)
            return;
        const float engageR2 = engageR * engageR;

        struct UnitPos
        {
            float x;
            float z;
            uint8_t team;
        };

        std::vector<UnitPos> units;
        units.reserve(256);

        const auto &q0 = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q0.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasPosition() || !st->hasHealth() || !st->hasTeam())
                continue;

            const auto &pos = st->positions();
            const auto &hp = st->healths();
            const auto &tm = st->teams();
            const uint32_t n = st->size();
            for (uint32_t row = 0; row < n; ++row)
            {
                if (hp[row].value <= 0.0f)
                    continue;
                units.push_back(UnitPos{pos[row].x, pos[row].z, tm[row].id});
            }
        }

        bool shouldStart = false;
        for (size_t i = 0; i < units.size() && !shouldStart; ++i)
        {
            for (size_t j = i + 1; j < units.size(); ++j)
            {
                if (units[i].team == units[j].team)
                    continue;
                const float dx = units[i].x - units[j].x;
                const float dz = units[i].z - units[j].z;
                const float d2 = dx * dx + dz * dz;
                if (d2 <= engageR2)
                {
                    shouldStart = true;
                    break;
                }
            }
        }

        if (!shouldStart)
            return;

        startBattle();
    }

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

    const float meleeRange = std::max(0.0f, m_cfg.meleeRange);
    const float meleeRange2 = meleeRange * meleeRange;
    const float engageRange = std::max(0.0f, m_cfg.engageRange);
    const float engageRange2 = engageRange * engageRange;
    const float disengageBuffer = 1.25f; // meters; prevents stop/chase flip-flop near range boundary
    const float disengageRange = meleeRange + disengageBuffer;
    const float disengageRange2 = disengageRange * disengageRange;

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

    // Clear per-frame buffers
    m_stops.clear();
    m_moves.clear();
    m_attackAnims.clear();
    m_damages.clear();
    m_damageAnims.clear();

    // ---- Phase 2: Per-entity combat decisions (no structural mutations) ----
    struct WorkItem
    {
        uint32_t archetypeId;
        uint32_t row;
        Engine::ECS::Entity entity;
        uint64_t selfKey;
    };

    std::vector<WorkItem> work;
    work.reserve(512);
    std::vector<UnitCombatMemory> prevMem;
    prevMem.reserve(512);

    // Build a stable work list of living units, and snapshot previous combat memory.
    for (uint32_t archetypeId : q.matchingArchetypeIds)
    {
        auto *storePtr = ecs.stores.get(archetypeId);
        if (!storePtr || !storePtr->hasPosition() || !storePtr->hasHealth() || !storePtr->hasTeam() ||
            !storePtr->hasAttackCooldown() || !storePtr->hasRenderAnimation() || !storePtr->hasFacing() ||
            !storePtr->hasMoveTarget())
        {
            continue;
        }

        const auto &healths = storePtr->healths();
        const auto &ents = storePtr->entities();
        const uint32_t n = storePtr->size();
        for (uint32_t row = 0; row < n; ++row)
        {
            if (healths[row].value <= 0.0f)
                continue;

            const Engine::ECS::Entity e = ents[row];
            const uint64_t key = entityKey(e);
            work.push_back(WorkItem{archetypeId, row, e, key});

            UnitCombatMemory mem{};
            auto it = m_unitMem.find(key);
            if (it != m_unitMem.end())
                mem = it->second;
            prevMem.push_back(mem);
        }
    }

    const uint32_t workCount = static_cast<uint32_t>(work.size());

    // Output per-item actions in stable order (deterministic merge).
    std::vector<uint8_t> hasStop(workCount, 0);
    std::vector<StopAction> stopOut(workCount);
    std::vector<uint8_t> hasMove(workCount, 0);
    std::vector<MoveAction> moveOut(workCount);
    std::vector<uint8_t> hasAttackAnim(workCount, 0);
    std::vector<AnimAction> attackAnimOut(workCount);
    std::vector<uint8_t> hasDamage(workCount, 0);
    std::vector<DamageAction> damageOut(workCount);
    std::vector<uint8_t> hasDamageAnim(workCount, 0);
    std::vector<AnimAction> damageAnimOut(workCount);
    std::vector<UnitCombatMemory> nextMem(workCount);

    const bool chargeActiveForDecisions = m_chargeActive;

    auto splitmix64 = [](uint64_t &x) -> uint64_t
    {
        uint64_t z = (x += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    };

    auto decideOne = [&](uint32_t /*workerIndex*/, uint32_t idx)
    {
        const WorkItem &wi = work[idx];
        auto *storePtr = ecs.stores.get(wi.archetypeId);
        if (!storePtr)
            return;

        auto &cooldowns = storePtr->attackCooldowns();
        const auto &pos = storePtr->positions();
        const auto &healths = storePtr->healths();
        const auto &teams = storePtr->teams();
        const auto &anims = storePtr->renderAnimations();

        // Tick cooldown
        cooldowns[wi.row].timer = std::max(0.0f, cooldowns[wi.row].timer - dt);

        // Optional human gating (currently unused by SampleApp)
        if (m_humanTeamId >= 0 && teams[wi.row].id == static_cast<uint8_t>(m_humanTeamId) && !m_humanAttacking)
            return;

        const Engine::ECS::Entity myEntity = wi.entity;
        const float myX = pos[wi.row].x;
        const float myZ = pos[wi.row].z;
        const uint8_t myTeam = teams[wi.row].id;

        UnitCombatMemory mem = prevMem[idx];
        mem.lastSeenFrame = m_frameCounter;

        // Deterministic per-entity RNG (stable across thread counts).
        uint64_t rngState = wi.selfKey ^ (static_cast<uint64_t>(m_frameCounter) * 0xd1342543de82ef95ull);
        auto rand01 = [&]() -> float
        {
            const uint64_t v = splitmix64(rngState);
            const float u = static_cast<float>(v & 0xFFFFFFu) / float(0x1000000u);
            return u;
        };
        auto randSigned = [&]() -> float
        {
            return rand01() * 2.0f - 1.0f;
        };
        auto randU32 = [&]() -> uint32_t
        {
            return static_cast<uint32_t>(splitmix64(rngState));
        };

        Engine::ECS::Entity bestEnemy{};
        float bestEX = myX;
        float bestEZ = myZ;
        float bestDist2 = CombatTuning::BEST_DIST2_INIT;

        // Spatial lookup: nearest enemy in neighborhood
        if (m_spatial)
        {
            m_spatial->forNeighbors(myX, myZ, [&](uint32_t otherArchId, uint32_t otherRow)
                                    {
                                        auto *os = ecs.stores.get(otherArchId);
                                        if (!os || !os->hasPosition() || !os->hasHealth() || !os->hasTeam())
                                            return;
                                        if (otherRow >= os->size())
                                            return;

                                        if (os->healths()[otherRow].value <= 0.0f)
                                            return;
                                        if (os->teams()[otherRow].id == myTeam)
                                            return;

                                        const float ex = os->positions()[otherRow].x;
                                        const float ez = os->positions()[otherRow].z;
                                        const float dx = ex - myX;
                                        const float dz = ez - myZ;
                                        const float d2 = dx * dx + dz * dz;

                                        const Engine::ECS::Entity cand = os->entities()[otherRow];
                                        const float tieEps = CombatTuning::ENEMY_TIE_EPS;
                                        const bool better = (d2 < bestDist2 - tieEps) ||
                                                            (std::fabs(d2 - bestDist2) <= tieEps &&
                                                             (!bestEnemy.valid() || cand.index < bestEnemy.index));
                                        if (better)
                                        {
                                            bestDist2 = d2;
                                            bestEX = ex;
                                            bestEZ = ez;
                                            bestEnemy = cand;
                                        } });
        }

        // Fallback: full scan only if spatial found nothing
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
                    if (otherArchId == wi.archetypeId && oRow == wi.row)
                        continue;
                    if (oTeam[oRow].id == myTeam)
                        continue;
                    if (oHP[oRow].value <= 0.0f)
                        continue;

                    const float dx = oPos[oRow].x - myX;
                    const float dz = oPos[oRow].z - myZ;
                    const float d2 = dx * dx + dz * dz;

                    const Engine::ECS::Entity cand = oEnt[oRow];
                    const float tieEps = CombatTuning::ENEMY_TIE_EPS;
                    const bool better = (d2 < bestDist2 - tieEps) ||
                                        (std::fabs(d2 - bestDist2) <= tieEps &&
                                         (!bestEnemy.valid() || cand.index < bestEnemy.index));
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
            // No enemies exist anywhere. Do not stomp player move targets.
            // Only stop units that were previously chasing due to combat.
            if (mem.combatMoveIssued)
            {
                float yaw = storePtr->facings()[wi.row].yaw;
                const bool clearVel = (storePtr->moveTargets()[wi.row].active != 0);
                hasStop[idx] = 1;
                stopOut[idx] = StopAction{myEntity, yaw, clearVel};
                mem.combatMoveIssued = false;
            }

            mem.targetEnemy = Engine::ECS::Entity{};
            mem.engaged = false;
            mem.inMelee = false;
            nextMem[idx] = mem;
            return;
        }

        // Decide whether to keep last target (stickiness) to avoid rapid swapping.
        Engine::ECS::Entity chosenEnemy = bestEnemy;
        float chosenEX = bestEX;
        float chosenEZ = bestEZ;
        float chosenDist2 = bestDist2;

        if (mem.targetEnemy.valid())
        {
            const auto *trec = ecs.entities.find(mem.targetEnemy);
            if (trec)
            {
                auto *tst = ecs.stores.get(trec->archetypeId);
                if (tst && trec->row < tst->size() && tst->hasPosition() && tst->hasHealth() && tst->hasTeam())
                {
                    if (tst->healths()[trec->row].value > 0.0f && tst->teams()[trec->row].id != myTeam)
                    {
                        const float tx = tst->positions()[trec->row].x;
                        const float tz = tst->positions()[trec->row].z;
                        const float tdx = tx - myX;
                        const float tdz = tz - myZ;
                        const float tdist2 = tdx * tdx + tdz * tdz;

                        const float switchFrac = 0.15f;
                        const bool keepOld = !(bestDist2 < tdist2 * (1.0f - switchFrac));
                        if (keepOld)
                        {
                            chosenEnemy = mem.targetEnemy;
                            chosenEX = tx;
                            chosenEZ = tz;
                            chosenDist2 = tdist2;
                        }
                    }
                }
            }
        }

        mem.targetEnemy = chosenEnemy;

        // Local engagement gating: only units that have an enemy within engageRange
        // participate in combat (chase/attack). This prevents global-aggro.
        // Once engaged, allow a small hysteresis so units don't instantly drop combat.
        const float engageHysteresis = 2.5f; // meters
        const float disengageEngageRange = std::max(0.0f, engageRange + engageHysteresis);
        const float disengageEngageRange2 = disengageEngageRange * disengageEngageRange;
        const bool hasNearbyEnemy = (engageRange > 0.0f) ? (chosenDist2 <= engageRange2) : false;
        const bool keepEngaged = mem.engaged && (chosenDist2 <= disengageEngageRange2);
        if (!hasNearbyEnemy && !keepEngaged)
        {
            // Not part of any local fight.
            // If we were previously chasing due to combat, stop that chase now.
            if (mem.combatMoveIssued)
            {
                const float yaw = storePtr->facings()[wi.row].yaw;
                const bool clearVel = (storePtr->moveTargets()[wi.row].active != 0);
                hasStop[idx] = 1;
                stopOut[idx] = StopAction{myEntity, yaw, clearVel};
                mem.combatMoveIssued = false;
            }

            mem.targetEnemy = Engine::ECS::Entity{};
            mem.engaged = false;
            mem.inMelee = false;
            nextMem[idx] = mem;
            return;
        }

        const float dx = chosenEX - myX;
        const float dz = chosenEZ - myZ;
        const float yaw = (dx * dx + dz * dz > CombatTuning::YAW_DIST2_EPS)
                              ? std::atan2(dx, dz)
                              : storePtr->facings()[wi.row].yaw;

        // From here on, unit is considered engaged in combat.
        mem.engaged = true;

        const bool inMelee = mem.inMelee ? (chosenDist2 <= disengageRange2) : (chosenDist2 <= meleeRange2);
        if (inMelee)
        {
            // We're fighting in melee range.
            mem.combatMoveIssued = false;
            mem.inMelee = true;

            const bool clearVel = (storePtr->moveTargets()[wi.row].active != 0);
            hasStop[idx] = 1;
            stopOut[idx] = StopAction{myEntity, yaw, clearVel};

            if (cooldowns[wi.row].timer <= 0.0f)
            {
                float jitter = 1.0f + randSigned() * m_cfg.cooldownJitter;
                cooldowns[wi.row].timer = cooldowns[wi.row].interval * jitter;

                const uint32_t attackClip = CombatAnims::ATTACK_START;
                hasAttackAnim[idx] = 1;
                attackAnimOut[idx] = AnimAction{myEntity, attackClip, CombatTuning::ATTACK_ANIM_SPEED, false};

                // --- Roll hit / miss ---
                if (rand01() >= m_cfg.missChance)
                {
                    float baseDmg = m_cfg.damageMin + rand01() * (m_cfg.damageMax - m_cfg.damageMin);

                    float myHPFrac = healths[wi.row].value / m_cfg.maxHPPerUnit;
                    float rageMult = 1.0f + m_cfg.rageMaxBonus * (1.0f - std::clamp(myHPFrac, 0.0f, 1.0f));
                    baseDmg *= rageMult;

                    const bool isCrit = (rand01() < m_cfg.critChance);
                    if (isCrit)
                        baseDmg *= m_cfg.critMultiplier;

                    hasDamage[idx] = 1;
                    damageOut[idx] = DamageAction{chosenEnemy, baseDmg};

                    const uint32_t dmgCount = (CombatAnims::DAMAGE_END - CombatAnims::DAMAGE_START + 1);
                    const uint32_t dmgClip = CombatAnims::DAMAGE_START + (randU32() % dmgCount);
                    const float dmgAnimSpeed = isCrit ? CombatTuning::CRIT_DAMAGE_ANIM_SPEED : CombatTuning::DAMAGE_ANIM_SPEED;
                    hasDamageAnim[idx] = 1;
                    damageAnimOut[idx] = AnimAction{chosenEnemy, dmgClip, dmgAnimSpeed, false};
                }
            }
        }
        else
        {
            // Not in melee yet: chase toward the chosen enemy.
            mem.inMelee = false;

            bool skipChase = false;
            if (chargeActiveForDecisions)
            {
                const auto &tgt = storePtr->moveTargets()[wi.row];
                float tdx = tgt.x - m_battleClickX;
                float tdz = tgt.z - m_battleClickZ;
                skipChase = tgt.active && (tdx * tdx + tdz * tdz < CombatTuning::CLICK_TARGET_MATCH_DIST2);
            }
            if (!skipChase)
            {
                float selfR = 0.0f;
                if (storePtr->hasRadius())
                    selfR = std::max(0.0f, storePtr->radii()[wi.row].r);

                const float a = hashAngle(static_cast<uint32_t>(myEntity.index));
                const float ringR = std::max(0.0f, m_cfg.meleeRange * 0.85f + selfR);
                const float offX = std::cos(a) * ringR;
                const float offZ = std::sin(a) * ringR;

                hasMove[idx] = 1;
                moveOut[idx] = MoveAction{myEntity,
                                          chosenEX + offX, chosenEZ + offZ,
                                          true,
                                          yaw,
                                          CombatAnims::RUN,
                                          anims[wi.row].clipIndex != CombatAnims::RUN};
                mem.combatMoveIssued = true;
            }
        }

        nextMem[idx] = mem;
    };

    Engine::JobSystem *js = ecs.jobSystem;
    const bool canParallel = (js != nullptr) && (js->workerCount() > 0) &&
                             (workCount >= CombatTuning::PARALLEL_DECIDE_ENTITY_THRESHOLD);
    if (canParallel)
        js->parallelFor(workCount, decideOne);
    else
    {
        for (uint32_t i = 0; i < workCount; ++i)
            decideOne(0, i);
    }

    // Merge memory updates and per-item actions in stable order.
    bool anyEngagedThisFrame = false;
    for (uint32_t i = 0; i < workCount; ++i)
    {
        if (nextMem[i].lastSeenFrame == m_frameCounter)
        {
            auto &mem = m_unitMem[work[i].selfKey];
            mem = nextMem[i];
            anyEngagedThisFrame = anyEngagedThisFrame || mem.engaged;
        }
    }
    if (m_chargeActive && anyEngagedThisFrame)
        m_chargeActive = false;

    for (uint32_t i = 0; i < workCount; ++i)
    {
        if (hasStop[i])
            m_stops.push_back(stopOut[i]);
        if (hasMove[i])
            m_moves.push_back(moveOut[i]);
        if (hasAttackAnim[i])
            m_attackAnims.push_back(attackAnimOut[i]);
        if (hasDamage[i])
            m_damages.push_back(damageOut[i]);
        if (hasDamageAnim[i])
            m_damageAnims.push_back(damageAnimOut[i]);
    }

    auto &stops = m_stops;
    auto &moves = m_moves;
    auto &attackAnims = m_attackAnims;
    auto &damages = m_damages;
    auto &damageAnims = m_damageAnims;

    // Cleanup unit memory for entities not seen this frame.
    if (!m_unitMem.empty())
    {
        for (auto it = m_unitMem.begin(); it != m_unitMem.end();)
        {
            if (it->second.lastSeenFrame != m_frameCounter)
                it = m_unitMem.erase(it);
            else
                ++it;
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
            bool targetMoved = (dtx * dtx + dtz * dtz > CombatTuning::MOVE_TARGET_REPATH_DIST2) || !tgt.active;
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
