#pragma once

#include "ECS/SystemFormat.h"
#include "assets/AssetManager.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

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
    AnimationPlaybackSystem()
    {
        setRequiredNames({"RenderModel", "RenderAnimation"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "AnimationPlaybackSystem"; }

    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_renderAnimId = registry.ensureId("RenderAnimation");
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        if (!m_assets)
            return;

        if (dt == 0.0f)
            return;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *st = ecs.stores.get(archetypeId);
            if (!st || !st->hasRenderModel() || !st->hasRenderAnimation())
                continue;

            auto &models = st->renderModels();
            auto &anims = st->renderAnimations();
            const uint32_t n = st->size();

            for (uint32_t row = 0; row < n; ++row)
            {
                const Engine::ModelHandle handle = models[row].handle;
                Engine::ModelAsset *asset = m_assets->getModel(handle);
                if (!asset)
                    continue;

                auto &anim = anims[row];
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
                    const float delta = dt * anim.speed;
                    if (std::abs(delta) > 1e-9f)
                    {
                        anim.timeSec += delta;
                        changed = true;
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
    }

private:
    Engine::AssetManager *m_assets = nullptr;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_renderAnimId = Engine::ECS::ComponentRegistry::InvalidID;
};
