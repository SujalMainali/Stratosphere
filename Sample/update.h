#pragma once

#include "ECS/ECSContext.h"

#include "systems/CommandSystem.h"
#include "systems/SteeringSystem.h"
#include "systems/NavGrid.h"
#include "systems/NavGridBuilderSystem.h"
#include "systems/PathfindingSystem.h"
#include "systems/MovementSystem.h"
#include "systems/CharacterAnimationSystem.h"
#include "systems/PoseUpdateSystem.h"
#include "systems/RenderSystem.h"
#include "systems/SpatialIndexSystem.h"
#include "systems/LocalAvoidanceSystem.h"
#include "systems/CombatSystem.h"

namespace Engine
{
    class AssetManager;
    class Renderer;
    class Camera;
}

namespace Sample
{
    // Owns and runs Sample gameplay systems in a consistent order.
    class SystemRunner
    {
    public:
        void Initialize(Engine::ECS::ECSContext &ecs);
        void Update(Engine::ECS::ECSContext &ecs, float dtSeconds);

        void SetAssetManager(Engine::AssetManager *assets);
        void SetRenderer(Engine::Renderer *renderer);
        void SetCamera(Engine::Camera *camera);
        void SetGlobalMoveTarget(float x, float y, float z);

        /// Access combat system for HUD stats
        const CombatSystem &GetCombatSystem() const { return m_combat; }
        /// Mutable access for config loading
        CombatSystem &GetCombatSystemMut() { return m_combat; }

    private:
        bool m_initialized = false;

        CommandSystem m_command;
        SteeringSystem m_steering;
        MovementSystem m_movement;

        NavGrid m_navGrid;
        NavGridBuilderSystem m_navGridBuilder{&m_navGrid};
        PathfindingSystem m_pathfinding{&m_navGrid};

        SpatialIndexSystem m_spatialIndex{2.0f};
        LocalAvoidanceSystem m_localAvoidance{&m_spatialIndex};
        CombatSystem m_combat;

        CharacterAnimationSystem m_characterAnim;

        PoseUpdateSystem m_poseUpdate;

        RenderSystem m_renderModel;
    };
}
