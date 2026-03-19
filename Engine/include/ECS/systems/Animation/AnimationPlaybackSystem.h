#pragma once

#include "ECS/SystemFormat.h"
#include "assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <iostream>
#endif

// Engine-generic animation playback system.
//
// Responsibilities:
// - Validate clip index against model clip count.
// - Advance RenderAnimation::timeSec by dt * speed while playing.
// - Apply looping with wrap.
// - For one-shots (loop==false): stop when reaching clip duration.
class AnimationPlaybackSystem : public Engine::ECS::SystemBase
{
public:
    struct AnimationActivityPolicy
    {
        uint32_t keepAliveFrames = 8;
        bool allowOffscreenLoopAdvance = false;
        bool advanceOffscreenOneShots = true;
        bool forceFullRate = false;
    };

    AnimationPlaybackSystem()
    {
        setRequiredNames({"RenderModel", "RenderAnimation", "VisibilityState"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "AnimationPlaybackSystem"; }

    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }
    void setActivityPolicy(const AnimationActivityPolicy &policy) { m_policy = policy; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_renderAnimId = registry.ensureId("RenderAnimation");
        m_visibilityStateId = registry.ensureId("VisibilityState");
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (!m_assets)
            return;

        if (dt == 0.0f)
            return;

        ++m_frameCounter;
        m_lastStats = Stats{};

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasRenderModel() || !st->hasRenderAnimation() || !st->hasVisibilityState())
                continue;

            auto &models = st->renderModels();
            auto &anims = st->renderAnimations();
            const auto &visibility = st->visibilityState();
            const uint32_t n = st->size();

            for (uint32_t row = 0; row < n; ++row)
            {
                m_lastStats.totalAnimated += 1u;

                const Engine::ModelHandle handle = models[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                if (!asset)
                    continue;

                auto &anim = anims[row];
                const auto &vis = visibility[row];

                const bool isVisible = vis.visible;
                const bool isOneShot = !anim.loop;
                const bool oneShotActive = isOneShot && anim.playing && m_policy.advanceOffscreenOneShots;
                bool recentlyVisible = false;
                if (!isVisible && vis.visibleFrame > 0 && vis.lastTestFrame >= vis.visibleFrame)
                {
                    const uint32_t framesSinceVisible = vis.lastTestFrame - vis.visibleFrame;
                    recentlyVisible = (framesSinceVisible <= m_policy.keepAliveFrames);
                }

                const bool shouldAdvance =
                    m_policy.forceFullRate ||
                    isVisible ||
                    (m_policy.allowOffscreenLoopAdvance && !isOneShot) ||
                    oneShotActive ||
                    recentlyVisible;

                if (isVisible)
                    m_lastStats.visibleAnimated += 1u;

                bool changed = false;

                const uint32_t clipCount = static_cast<uint32_t>(asset->animClips.size());
                if (clipCount == 0)
                {
                    if (anim.clipIndex != 0 || anim.timeSec != 0.0f || anim.playing)
                    {
                        anim.clipIndex = 0;
                        anim.timeSec = 0.0f;
                        anim.playing = false;
                        changed = true;
                    }
                    if (changed)
                        ecs.markDirty(m_renderAnimId, archetypeId, row);
                    continue;
                }

                if (anim.clipIndex >= clipCount)
                {
                    anim.clipIndex = clipCount - 1;
                    anim.timeSec = 0.0f;
                    changed = true;
                }

                const float duration = asset->animClips[anim.clipIndex].durationSec;
                if (duration <= 1e-6f)
                {
                    if (anim.timeSec != 0.0f || anim.playing)
                    {
                        anim.timeSec = 0.0f;
                        anim.playing = false;
                        changed = true;
                    }
                    if (changed)
                        ecs.markDirty(m_renderAnimId, archetypeId, row);
                    continue;
                }

                if (anim.playing)
                {
                    if (shouldAdvance)
                    {
                        const float delta = dt * anim.speed;
                        if (std::abs(delta) > 1e-9f)
                        {
                            anim.timeSec += delta;
                            changed = true;
                            m_lastStats.playbackAdvanced += 1u;
                        }

                        if (anim.loop)
                        {
                            anim.timeSec = std::fmod(anim.timeSec, duration);
                            if (anim.timeSec < 0.0f)
                                anim.timeSec += duration;
                        }
                        else
                        {
                            if (anim.timeSec >= duration)
                            {
                                anim.timeSec = duration;
                                anim.playing = false;
                                changed = true;
                            }
                            else if (anim.timeSec < 0.0f)
                            {
                                anim.timeSec = 0.0f;
                                changed = true;
                            }
                        }
                    }
                    else
                    {
                        m_lastStats.skippedInvisible += 1u;
                    }
                }
                else
                {
                    // Ensure time is in-range even when not playing.
                    if (anim.timeSec < 0.0f)
                    {
                        anim.timeSec = 0.0f;
                        changed = true;
                    }
                    if (anim.timeSec > duration)
                    {
                        anim.timeSec = duration;
                        changed = true;
                    }
                }

                if (changed)
                    ecs.markDirty(m_renderAnimId, archetypeId, row);
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            std::cout << "[AnimationPlaybackSystem] frame=" << m_frameCounter
                      << " total=" << m_lastStats.totalAnimated
                      << " visible=" << m_lastStats.visibleAnimated
                      << " advanced=" << m_lastStats.playbackAdvanced
                      << " skippedInvisible=" << m_lastStats.skippedInvisible
                      << "\n";
        }
#endif
    }

private:
    struct Stats
    {
        uint32_t totalAnimated = 0;
        uint32_t visibleAnimated = 0;
        uint32_t playbackAdvanced = 0;
        uint32_t skippedInvisible = 0;
    };

    Engine::AssetManager *m_assets = nullptr;
    AnimationActivityPolicy m_policy{};

    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_visibilityStateId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_frameCounter = 0;
    Stats m_lastStats{};
};
