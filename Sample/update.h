#pragma once

#include "ECS/ECSContext.h"

#include "ECS/systems/CommandSystem.h"
#include "ECS/systems/SteeringSystem.h"
#include "ECS/systems/NavGrid.h"
#include "ECS/systems/NavGridBuilderSystem.h"
#include "ECS/systems/PathfindingSystem.h"
#include "ECS/systems/MovementSystem.h"
#include "ECS/systems/RenderTransformUpdateSystem.h"
#include "ECS/systems/RenderBoundsUpdateSystem.h"
#include "ECS/systems/VisibilityCullingSystem.h"
#include "systems/LocomotionAnimationControllerSystem.h"
#include "ECS/systems/Animation/AnimationPlaybackSystem.h"
#include "ECS/systems/PoseUpdateSystem.h"
#include "ECS/systems/RenderSystem.h"
#include "ECS/systems/SpatialIndexSystem.h"
#include "ECS/systems/LocalAvoidanceSystem.h"
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

        /// Access spatial index for fast proximity queries (selection, combat helpers, etc.)
        const SpatialIndexSystem &GetSpatialIndex() const { return m_spatialIndex; }
        SpatialIndexSystem &GetSpatialIndexMut() { return m_spatialIndex; }

        /// Reset all systems for a clean restart (clears cached queries, battle state, etc.)
        void ResetForRestart(Engine::ECS::ECSContext &ecs);

    private:
        bool m_initialized = false;

        CommandSystem m_command;
        SteeringSystem m_steering;
        MovementSystem m_movement;
        RenderTransformUpdateSystem m_renderTransform;
        RenderBoundsUpdateSystem m_renderBoundsUpdate;
        VisibilityCullingSystem m_visibilityCulling;

        NavGrid m_navGrid;
        NavGridBuilderSystem m_navGridBuilder{&m_navGrid};
        PathfindingSystem m_pathfinding{&m_navGrid};

        SpatialIndexSystem m_spatialIndex{2.0f};
        LocalAvoidanceSystem m_localAvoidance{&m_spatialIndex};
        CombatSystem m_combat;

        LocomotionAnimationControllerSystem m_locomotionAnim;
        AnimationPlaybackSystem m_animPlayback;

        PoseUpdateSystem m_poseUpdate;

        RenderSystem m_renderModel;
    };
}
