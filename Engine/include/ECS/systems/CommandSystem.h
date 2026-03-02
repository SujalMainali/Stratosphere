#pragma once

#include "ECS/SystemFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

class CommandSystem : public Engine::ECS::SystemBase
{
public:
    struct Config
    {
        float spacing = 0.5f;
        float minWorld = -10000.0f;
        float maxWorld = 10000.0f;
        bool log = false;
    };

    CommandSystem()
    {
        // Only command selected, movable units.
        setRequiredNames({"MoveTarget", "MoveSpeed", "Selected"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "CommandSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_moveTargetId = registry.ensureId("MoveTarget");
    }

    void setConfig(const Config &cfg) { m_cfg = cfg; }

    // Set the last clicked target; system will write it to entities on next update.
    void SetGlobalMoveTarget(float x, float y, float z)
    {
        m_hasPending = true;
        m_pendingX = x;
        m_pendingY = y;
        m_pendingZ = z;
    }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (!m_hasPending)
            return;

        const float spacing = m_cfg.spacing;
        const float kMinWorld = m_cfg.minWorld;
        const float kMaxWorld = m_cfg.maxWorld;

        auto clamp = [](float v, float a, float b)
        { return std::max(a, std::min(v, b)); };

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        const auto &q = ecs.queries.get(m_queryId);
        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            Engine::ECS::ArchetypeStore *storePtr = ecs.stores.get(archetypeId);
            if (!storePtr)
                continue;
            auto &store = *storePtr;

            auto &targets = const_cast<std::vector<Engine::ECS::MoveTarget> &>(store.moveTargets());
            const uint32_t selCount = store.size();
            if (selCount == 0)
                continue;

            const uint32_t side = static_cast<uint32_t>(std::ceil(std::sqrt(static_cast<float>(selCount))));
            const float half = (static_cast<float>(side) - 1.0f) * 0.5f;

            for (uint32_t k = 0; k < selCount; ++k)
            {
                const uint32_t row = k / side;
                const uint32_t col = k % side;
                const float ox = (static_cast<float>(col) - half) * spacing;
                const float oz = (static_cast<float>(row) - half) * spacing;

                targets[k].x = clamp(m_pendingX + ox, kMinWorld, kMaxWorld);
                targets[k].y = m_pendingY; // height
                targets[k].z = clamp(m_pendingZ + oz, kMinWorld, kMaxWorld);
                targets[k].active = 1;

                // Mark MoveTarget dirty so SteeringSystem picks up the change.
                ecs.markDirty(m_moveTargetId, archetypeId, k);
            }

            // Optional logging hook for samples.
            if (m_cfg.log)
                onLog(selCount, side, spacing);
        }

        m_hasPending = false;
    }

protected:
    // Samples can override or ignore by keeping log disabled.
    virtual void onLog(uint32_t /*selectedCount*/, uint32_t /*gridSide*/, float /*spacing*/) {}

private:
    Config m_cfg{};

    bool m_hasPending = false;
    float m_pendingX = 0.0f, m_pendingY = 0.0f, m_pendingZ = 0.0f;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_moveTargetId = Engine::ECS::ComponentRegistry::InvalidID;
};
