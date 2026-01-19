#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // Mesh Record (V1)
    // ============================================================
    // This record points to raw VB/IB byte ranges inside the blob section.
    // Runtime will upload these bytes into GPU-local buffers.
    struct SModelMeshRecord
    {
        // Offset into string table (0 means "no name")
        uint32_t nameStrOffset;

        uint32_t vertexStride; // bytes per vertex (ex: 32, 48, 64)

        uint32_t vertexCount; // number of vertices in VB
        uint32_t indexCount;  // number of indices in IB

        uint32_t layoutFlags; // VertexLayoutFlags bitmask
        uint32_t indexType;   // IndexType (U16/U32)

        // Blob offsets are relative to header.blobOffset
        uint64_t vertexDataOffset; // start of vertex bytes
        uint64_t vertexDataSize;   // size of vertex bytes in blob

        uint64_t indexDataOffset; // start of index bytes
        uint64_t indexDataSize;   // size of index bytes in blob

        // Simple bounds (for culling / camera fitting later)
        float aabbMin[3];
        float aabbMax[3];
    };

#pragma pack(pop)

    static_assert(sizeof(SModelMeshRecord) == 80, "SModelMeshRecord size mismatch");

} // namespace Engine::smodel