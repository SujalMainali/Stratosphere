#pragma once
#include <vector>
#include <cstdint>
#include "assets/Handles.h" // MeshHandle, MaterialHandle

namespace Engine
{
    // ------------------------------------------------------------
    // ModelAsset (CPU-only)
    // ------------------------------------------------------------
    // A model = list of primitives (mesh + material + draw range).
    // Later, renderer will iterate primitives and draw them.
    struct ModelPrimitive
    {
        MeshHandle mesh{};
        MaterialHandle material{};

        uint32_t firstIndex = 0;
        uint32_t indexCount = 0;
        int32_t vertexOffset = 0;
    };

    struct ModelAsset
    {
        std::vector<ModelPrimitive> primitives;

        // Optional debug name (string table later)
        const char *debugName = "";
    };

} // namespace Engine
