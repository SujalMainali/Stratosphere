#pragma once

#include "ECS/SystemFormat.h"
#include <glm/glm.hpp>
#include <cmath>

class RenderBoundsUpdateSystem : public Engine::ECS::SystemBase
{
public:
    RenderBoundsUpdateSystem()
    {
        // Requires render transform (world matrix) and render bounds (sphere data)
        setRequiredNames({"RenderTransform", "RenderBounds"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderBoundsUpdateSystem"; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
        m_queryId = Engine::ECS::QueryManager::InvalidQuery;
    }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;

        if (m_queryId == Engine::ECS::QueryManager::InvalidQuery)
            m_queryId = ecs.queries.createQuery(required(), excluded(), ecs.stores);

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
            if (!store.hasRenderTransform() || !store.hasRenderBounds())
                continue;

            auto &renderTransforms = store.renderTransforms();
            auto &renderBounds = store.renderBounds();
            const uint32_t n = store.size();

            for (uint32_t row = 0; row < n; ++row)
            {
                auto &bounds = renderBounds[row];
                const auto &transform = renderTransforms[row].world;

                // Transform local sphere center to world space
                glm::vec4 localCenter4{bounds.localCenter, 1.0f};
                glm::vec4 worldCenter4 = transform * localCenter4;
                bounds.worldCenter = glm::vec3(worldCenter4);

                // Scale the radius by the maximum scale component in the transform
                // For uniform scaling, any axis works. For non-uniform, use max to be conservative.
                float scaleX = glm::length(glm::vec3(transform[0]));
                float scaleY = glm::length(glm::vec3(transform[1]));
                float scaleZ = glm::length(glm::vec3(transform[2]));
                float maxScale = std::max({scaleX, scaleY, scaleZ});

                bounds.worldRadius = bounds.localRadius * maxScale;
                bounds.boundsVersion++;
            }
        }
    }

private:
    Engine::ECS::QueryId m_queryId = Engine::ECS::QueryManager::InvalidQuery;
};
