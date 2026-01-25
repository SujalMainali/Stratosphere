#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)
    // Binary record describing a scene node for .smodel V2
    struct SModelNodeRecord
    {
        // String table offsets
        uint32_t nameStrOffset; // 0 = empty

        // Hierarchy
        uint32_t parentIndex; // UINT32_MAX for root
        uint32_t firstChild;  // UINT32_MAX if none
        uint32_t childCount;  // 0 if none

        // Primitive range into NodePrimitiveIndices[]
        uint32_t firstPrimitiveIndex; // start index into nodePrimitiveIndices
        uint32_t primitiveCount;      // number of indices for this node

        // Local transform (column-major 4x4)
        float localMatrix[16];
    };
#pragma pack(pop)

    static_assert(sizeof(SModelNodeRecord) == 88, "SModelNodeRecord size mismatch");
}
