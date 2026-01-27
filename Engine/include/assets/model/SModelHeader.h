#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // .smodel Header (V3.x)
    // ============================================================
    // The header contains:
    // - counts of record arrays
    // - absolute offsets to each section
    // - sizes of string table and blob section
    //
    // All offsets are absolute byte offsets from the start of the file.
    // Blob offsets inside records are relative to header.blobOffset.
    //
    // Magic: 'SMOD' = 0x444F4D53 (little-endian)
    // bytes: 53 4D 4F 44
    struct SModelHeader
    {
        uint32_t magic;        // must equal 'SMOD'
        uint16_t versionMajor; // 3
        uint16_t versionMinor; // 0

        uint32_t fileSizeBytes; // entire file size (validation)
        uint32_t flags;         // reserved for future use (0 for v1)

        // Counts for each record table
        uint32_t meshCount;      // number of mesh records (VB/IB blobs)
        uint32_t primitiveCount; // number of draw primitives (mesh+material)
        uint32_t materialCount;  // number of material records
        uint32_t textureCount;   // number of texture records
        // NEW: node graph
        uint32_t nodeCount;               // number of node records
        uint32_t nodePrimitiveIndexCount; // number of node->primitive index entries

        // Absolute offsets to record tables (from file start)
        uint64_t meshesOffset;
        uint64_t primitivesOffset;
        uint64_t materialsOffset;
        uint64_t texturesOffset;
        // NEW: nodes + node primitive index table offsets
        uint64_t nodesOffset;
        uint64_t nodePrimitiveIndicesOffset;

        // Absolute offset to string table and blob section
        uint64_t stringTableOffset;
        uint64_t blobOffset;

        // Sizes of those sections
        uint64_t stringTableSize;
        uint64_t blobSize;

        // NEW in v2.1: explicit direct-children list.
        // Offsets are absolute byte offsets from file start (like other offsets).
        // Stored as uint32_t to preserve the header's fixed 128-byte size.
        uint32_t nodeChildIndicesOffset; // 0 if absent
        uint32_t nodeChildIndicesCount;  // number of uint32 entries

        // NEW in v3.0: animation sections (optional; counts can be 0)
        uint32_t animClipsOffset;
        uint32_t animClipsCount;

        uint32_t animChannelsOffset;
        uint32_t animChannelsCount;

        uint32_t animSamplersOffset;
        uint32_t animSamplersCount;

        uint32_t animTimesOffset; // float seconds
        uint32_t animTimesCount;  // number of floats

        uint32_t animValuesOffset; // float packed values
        uint32_t animValuesCount;  // number of floats
    };

#pragma pack(pop)

    // Size must remain stable across tool/runtime.
    static_assert(sizeof(SModelHeader) == 168, "SModelHeader size mismatch");

} // namespace Engine::smodel