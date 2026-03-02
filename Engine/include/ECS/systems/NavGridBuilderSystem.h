#pragma once
/*
  NavGridBuilderSystem
  --------------------
  Purpose:
    - Scans all entities with Obstacle + ObstacleRadius components.
    - Marks their blocked cells in the NavGrid.
    - Runs ONCE at init (or on demand), not every frame ideally.
*/

#include "ECS/SystemFormat.h"
#include "ECS/systems/NavGrid.h"

#include <algorithm>

class NavGridBuilderSystem : public Engine::ECS::SystemBase
{
public:
    struct Config
    {
        // Extra radius inflation beyond ObstacleRadius.
        // (Sample can set this to keep units from clipping on coarse grids.)
        float extraInflation = 0.0f;
    };

    NavGridBuilderSystem(NavGrid *grid)
        : m_grid(grid)
    {
        setRequiredNames({"Position", "Obstacle", "ObstacleRadius"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "NavGridBuilderSystem"; }

    void setConfig(const Config &cfg) { m_cfg = cfg; }

    void update(Engine::ECS::ECSContext &ecs, float /*dt*/) override
    {
        if (!m_grid)
            return;

        if (!m_grid->dirty)
            return;

        std::fill(m_grid->blocked.begin(), m_grid->blocked.end(), 0);

        for (const auto &ptr : ecs.stores.stores())
        {
            if (!ptr)
                continue;
            auto &store = *ptr;

            if (!store.signature().containsAll(required()))
                continue;
            if (!store.signature().containsNone(excluded()))
                continue;

            auto &positions = store.positions();
            auto &radii = store.obstacleRadii();
            const uint32_t n = store.size();

            for (uint32_t i = 0; i < n; ++i)
                m_grid->markObstacle(positions[i].x, positions[i].z, radii[i].r + m_cfg.extraInflation);
        }

        m_grid->dirty = false;
    }

private:
    Config m_cfg{};
    NavGrid *m_grid = nullptr;
};
