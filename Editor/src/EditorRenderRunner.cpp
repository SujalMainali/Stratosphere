#include "EditorRenderRunner.h"

#include "ECS/ECSContext.h"

#include "assets/AssetManager.h"
#include "Engine/Renderer.h"
#include "Engine/Camera.h"

namespace Editor
{

    void EditorRenderRunner::SetAssetManager(Engine::AssetManager *assets)
    {
        m_animPlayback.setAssetManager(assets);
        m_poseUpdate.setAssetManager(assets);
        m_render.setAssetManager(assets);
    }

    void EditorRenderRunner::SetRenderer(Engine::Renderer *renderer)
    {
        m_render.setRenderer(renderer);
    }

    void EditorRenderRunner::SetCamera(Engine::Camera *camera)
    {
        m_render.setCamera(camera);
    }

    void EditorRenderRunner::Initialize(Engine::ECS::ECSContext &ecs)
    {
        if (m_initialized)
            return;

        ecs.WireQueryManager();

        auto &registry = ecs.components;

        m_renderTransform.buildMasks(registry);
        m_animPlayback.buildMasks(registry);
        m_poseUpdate.buildMasks(registry);
        m_render.buildMasks(registry);

        m_initialized = true;
    }

    void EditorRenderRunner::Update(Engine::ECS::ECSContext &ecs, float dtSeconds)
    {
        if (!m_initialized)
            Initialize(ecs);

        // Keep scene stable: no locomotion/combat/etc.
        // We still update render transforms + pose + render batches.
        m_renderTransform.update(ecs, 0.0f);

        // Allow optional animation playback when dt>0.
        if (dtSeconds > 0.0f)
            m_animPlayback.update(ecs, dtSeconds);

        m_poseUpdate.update(ecs, 0.0f);
        m_render.update(ecs, 0.0f);
    }

}
