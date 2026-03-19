#pragma once

#include "ECS/SystemFormat.h"
#include "Engine/Frustum.h"
#include "Engine/Camera.h"

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <iostream>
#endif

class VisibilityCullingSystem : public Engine::ECS::SystemBase
{
public:
    VisibilityCullingSystem()
    {
        // Requires render bounds and visibility state
        setRequiredNames({"RenderBounds", "VisibilityState"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "VisibilityCullingSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_visibilityStateId = registry.ensureId("VisibilityState");
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void setCamera(Engine::Camera *camera) { m_camera = camera; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;

        if (!m_camera)
            return;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

        m_frameCounter++;
        m_lastStats = Stats{};

        // Build frustum from camera
        glm::mat4 viewProj = m_camera->GetProjectionMatrix() * m_camera->GetViewMatrix();
        Engine::Frustum frustum = Engine::Frustum::fromViewProjection(viewProj);

        const auto &q = ecs.queries.get(m_queryId);

        for (uint32_t archetypeId : q.matchingArchetypeIds)
        {
            auto *ptr = ecs.stores.get(archetypeId);
            if (!ptr)
                continue;

            auto &store = *ptr;
            if (!store.signature().containsAll(required()))
                continue;
            if (!store.signature().containsNone(excluded()))
                continue;
            if (!store.hasRenderBounds() || !store.hasVisibilityState())
                continue;

            auto &renderBounds = store.renderBounds();
            auto &visibilityStates = store.visibilityState();
            const uint32_t n = store.size();

            for (uint32_t row = 0; row < n; ++row)
            {
                auto &visibility = visibilityStates[row];
                const auto &bounds = renderBounds[row];

                m_lastStats.totalTested += 1u;

                // Store previous visibility for transition detection
                const bool prevVisible = visibility.visible;
                visibility.wasVisibleLastFrame = prevVisible;

                // Test sphere against frustum
                visibility.visible = frustum.testSphere(bounds.worldCenter, bounds.worldRadius);

                // Track when visibility changes
                visibility.lastTestFrame = m_frameCounter;

                if (visibility.visible)
                {
                    visibility.visibleFrame = m_frameCounter;
                    m_lastStats.visibleNow += 1u;
                    if (!prevVisible)
                        m_lastStats.becameVisible += 1u;
                }
                else if (prevVisible)
                {
                    m_lastStats.becameInvisible += 1u;
                }

                if (visibility.visible != prevVisible)
                {
                    ecs.markDirty(m_visibilityStateId, archetypeId, row);
                }
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            std::cout << "[VisibilityCullingSystem] frame=" << m_frameCounter
                      << " tested=" << m_lastStats.totalTested
                      << " visible=" << m_lastStats.visibleNow
                      << " becameVisible=" << m_lastStats.becameVisible
                      << " becameInvisible=" << m_lastStats.becameInvisible
                      << "\n";
        }
#endif
    }

    uint32_t getFrameCounter() const { return m_frameCounter; }

private:
    struct Stats
    {
        uint32_t totalTested = 0;
        uint32_t visibleNow = 0;
        uint32_t becameVisible = 0;
        uint32_t becameInvisible = 0;
    };

    Engine::Camera *m_camera = nullptr;
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    uint32_t m_visibilityStateId = Engine::ECS::ComponentRegistry::InvalidID;
    uint32_t m_frameCounter = 0;
    Stats m_lastStats{};
};
