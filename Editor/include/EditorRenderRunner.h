#pragma once

#include "ECS/systems/RenderTransformUpdateSystem.h"
#include "ECS/systems/Animation/AnimationPlaybackSystem.h"
#include "ECS/systems/PoseUpdateSystem.h"
#include "ECS/systems/RenderSystem.h"

namespace Engine
{
    class AssetManager;
    class Renderer;
    class Camera;
}

namespace Engine::ECS
{
    struct ECSContext;
}

namespace Editor
{
    // Minimal system runner for the Editor: keeps the scene renderable
    // without running gameplay simulation (combat/movement/pathfinding/etc.).
    class EditorRenderRunner
    {
    public:
        void Initialize(Engine::ECS::ECSContext &ecs);
        void Update(Engine::ECS::ECSContext &ecs, float dtSeconds);

        void SetAssetManager(Engine::AssetManager *assets);
        void SetRenderer(Engine::Renderer *renderer);
        void SetCamera(Engine::Camera *camera);

    private:
        bool m_initialized = false;

        RenderTransformUpdateSystem m_renderTransform;
        AnimationPlaybackSystem m_animPlayback;
        PoseUpdateSystem m_poseUpdate;
        RenderSystem m_render;
    };
}
