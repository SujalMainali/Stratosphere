#pragma once

#include "ECS/Entity.h"
#include <assets/Handles.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace Engine::ECS
{
    // Lightweight reference to a visible renderable row.
    // This is frame-local data produced by VisibleRenderGatherSystem.
    struct VisibleRenderRef
    {
        Entity entity{};
        uint32_t archetypeId = UINT32_MAX;
        uint32_t row = UINT32_MAX;

        ModelHandle model{};
        uint64_t modelKey = 0;

        uint32_t transformVersion = 0;
        uint32_t poseVersion = 0;
    };

    struct VisibleModelBucket
    {
        ModelHandle handle{};
        uint32_t lastUsedFrame = 0;
        std::vector<VisibleRenderRef> refs;
    };

    // Visible render buckets for the current frame.
    // byModel persists across frames; activeModelKeys indicates which keys are active this frame.
    struct VisibleRenderBuckets
    {
        uint32_t frame = 0;

        uint32_t totalRenderables = 0;
        uint32_t visibleRenderables = 0;

        std::unordered_map<uint64_t, VisibleModelBucket> byModel;
        std::vector<uint64_t> activeModelKeys;
    };
}
