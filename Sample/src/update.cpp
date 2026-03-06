#include "update.h"

#include <chrono>

namespace Sample
{
    void SystemRunner::Initialize(Engine::ECS::ECSContext &ecs)
    {
        if (m_initialized)
            return;

        // Ensure queries get incrementally updated as new stores appear.
        ecs.WireQueryManager();

        auto &registry = ecs.components;

        // Ensure common IDs exist up-front (also used by scenario spawner selection).
        (void)registry.ensureId("Selected");

        m_command.buildMasks(registry);
        m_steering.buildMasks(registry);
        m_navGridBuilder.buildMasks(registry);
        {
            NavGridBuilderSystem::Config cfg;
            cfg.extraInflation = 2.5f;
            m_navGridBuilder.setConfig(cfg);
        }
        m_pathfinding.buildMasks(registry);
        m_movement.buildMasks(registry);
        m_renderTransform.buildMasks(registry);
        m_spatialIndex.buildMasks(registry);
        m_localAvoidance.buildMasks(registry);
        m_combat.buildMasks(registry);
        m_combat.setSpatialIndex(&m_spatialIndex);
        m_locomotionAnim.buildMasks(registry);
        m_animPlayback.buildMasks(registry);
        m_poseUpdate.buildMasks(registry);
        m_renderModel.buildMasks(registry);

        // Initialize NavGrid (cover map area)
        m_navGrid.rebuild(2.0f, -400.0f, -400.0f, 400.0f, 400.0f);

        m_initialized = true;
    }

    void SystemRunner::Update(Engine::ECS::ECSContext &ecs, float dtSeconds)
    {
        if (!m_initialized)
            Initialize(ecs);

        if (dtSeconds <= 0.0f)
            return;

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        // Per-frame schedule trace (debug-only).
        ecs.trace.beginFrame();

        auto runTraced = [&](const char *sysName, auto &&fn)
        {
            using Clock = std::chrono::high_resolution_clock;
            const auto t0 = Clock::now();

            ecs.queries.setCurrentSystemName(sysName);
            fn();
            ecs.queries.clearCurrentSystemName();

            const auto t1 = Clock::now();
            const float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

            // matchingArchetypes/entitiesScanned are filled in more deeply later;
            // dirtyRowsConsumed will be accumulated via QueryManager::consumeDirtyRows().
            ecs.trace.onSystemEnd(sysName, ms, 0u, 0u);
        };
#endif

        // 1. Input
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_command.name(), [&]()
                  { m_command.update(ecs, dtSeconds); });
#else
        m_command.update(ecs, dtSeconds);
#endif

        // 2. Spatial index rebuild (used by combat neighbor queries)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_spatialIndex.name(), [&]()
                  { m_spatialIndex.update(ecs, dtSeconds); });
#else
        m_spatialIndex.update(ecs, dtSeconds);
#endif

        // 3. Combat (may set move targets/stop units)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_combat.name(), [&]()
                  { m_combat.update(ecs, dtSeconds); });
#else
        m_combat.update(ecs, dtSeconds);
#endif

        // 4. NavGrid (Rebuild grid from static obstacles)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_navGridBuilder.name(), [&]()
                  { m_navGridBuilder.update(ecs, dtSeconds); });
#else
        m_navGridBuilder.update(ecs, dtSeconds);
#endif

        // 5. Pathfinding (Plan paths for units with invalid/new targets)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_pathfinding.name(), [&]()
                  { m_pathfinding.update(ecs, dtSeconds); });
#else
        m_pathfinding.update(ecs, dtSeconds);
#endif

        // 6. Steering (Follow waypoints, update facing) writes preferred velocity
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_steering.name(), [&]()
                  { m_steering.update(ecs, dtSeconds); });
#else
        m_steering.update(ecs, dtSeconds);
#endif

        // 7. Local avoidance adjusts velocity to reduce overlaps
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_localAvoidance.name(), [&]()
                  { m_localAvoidance.update(ecs, dtSeconds); });
#else
        m_localAvoidance.update(ecs, dtSeconds);
#endif

        // 8. Movement integration (uses velocity produced by steering+avoidance)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_movement.name(), [&]()
                  { m_movement.update(ecs, dtSeconds); });
#else
        m_movement.update(ecs, dtSeconds);
#endif

        // 9. Render transforms (world matrix + version for render caching)
        // m_renderTransform.update(ecs, dtSeconds);

        // 10. Animation selection (sample policy)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_locomotionAnim.name(), [&]()
                  { m_locomotionAnim.update(ecs, dtSeconds); });
#else
        m_locomotionAnim.update(ecs, dtSeconds);
#endif

        // 11. Animation playback (engine)
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_animPlayback.name(), [&]()
                  { m_animPlayback.update(ecs, dtSeconds); });
#else
        m_animPlayback.update(ecs, dtSeconds);
#endif

        // 12. Pose update
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_poseUpdate.name(), [&]()
                  { m_poseUpdate.update(ecs, dtSeconds); });
#else
        m_poseUpdate.update(ecs, dtSeconds);
#endif

        // 13. Render
#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        runTraced(m_renderModel.name(), [&]()
                  { m_renderModel.update(ecs, dtSeconds); });
#else
        m_renderModel.update(ecs, dtSeconds);
#endif
    }

    void SystemRunner::SetAssetManager(Engine::AssetManager *assets)
    {
        m_animPlayback.setAssetManager(assets);
        m_poseUpdate.setAssetManager(assets);
        m_renderModel.setAssetManager(assets);
        m_combat.setAssetManager(assets);
    }

    void SystemRunner::SetRenderer(Engine::Renderer *renderer)
    {
        m_renderModel.setRenderer(renderer);
    }

    void SystemRunner::SetCamera(Engine::Camera *camera)
    {
        m_renderModel.setCamera(camera);
    }

    void SystemRunner::SetGlobalMoveTarget(float x, float y, float z)
    {
        m_command.SetGlobalMoveTarget(x, y, z);
    }
}
