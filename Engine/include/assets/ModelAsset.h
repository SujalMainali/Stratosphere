#pragma once
#include <vector>
#include <cstdint>
#include <glm/glm.hpp>
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

        // Node graph (Option B)
        struct ModelNode
        {
            uint32_t parentIndex{~0u};
            uint32_t firstChild{~0u};
            uint32_t childCount{0};

            uint32_t firstPrimitiveIndex{0};
            uint32_t primitiveCount{0};

            glm::mat4 localMatrix{1.0f};
            glm::mat4 globalMatrix{1.0f};

            const char *debugName{nullptr};
        };

        std::vector<ModelNode> nodes;
        std::vector<uint32_t> nodePrimitiveIndices;
        uint32_t rootNodeIndex{0};

        // Optional debug name (string table later)
        const char *debugName = "";

        // Aggregate bounds across all meshes used by the model
        float boundsMin[3]{0.0f, 0.0f, 0.0f};
        float boundsMax[3]{0.0f, 0.0f, 0.0f};
        bool hasBounds = false;

        // Precomputed center and uniform scale to fit target size (e.g., 20 units)
        float center[3]{0.0f, 0.0f, 0.0f};
        float fitScale = 1.0f;
    };

} // namespace Engine
