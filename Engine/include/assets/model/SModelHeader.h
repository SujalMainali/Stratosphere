#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // .smodel Header (V1)
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
        uint16_t versionMajor; // 1
        uint16_t versionMinor; // 0

        uint32_t fileSizeBytes; // entire file size (validation)
        uint32_t flags;         // reserved for future use (0 for v1)

        // Counts for each record table
        uint32_t meshCount;      // number of mesh records (VB/IB blobs)
        uint32_t primitiveCount; // number of draw primitives (mesh+material)
        uint32_t materialCount;  // number of material records
        uint32_t textureCount;   // number of texture records

        uint32_t reserved0;

        // Absolute offsets to record tables (from file start)
        uint64_t meshesOffset;
        uint64_t primitivesOffset;
        uint64_t materialsOffset;
        uint64_t texturesOffset;

        // Absolute offset to string table and blob section
        uint64_t stringTableOffset;
        uint64_t blobOffset;

        // Sizes of those sections
        uint64_t stringTableSize;
        uint64_t blobSize;

        uint64_t reserved1;
    };

#pragma pack(pop)

    // Size must remain stable across tool/runtime.
    static_assert(sizeof(SModelHeader) == 108, "SModelHeader size mismatch");

} // namespace Engine::smodel