#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assets/ModelFormat.h" // umbrella include for SModel structs

namespace Engine::smodel
{
    // ------------------------------------------------------------
    // SModelFileView
    // ------------------------------------------------------------
    // Owns file bytes and provides typed views (pointers) into it.
    // AssetManager will use this to build GPU resources later.
    struct SModelFileView
    {
        std::vector<uint8_t> fileBytes; // owns the whole file memory

        // Header pointer inside fileBytes
        const SModelHeader *header = nullptr;

        // Record table pointers inside fileBytes
        const SModelMeshRecord *meshes = nullptr;
        const SModelPrimitiveRecord *primitives = nullptr;
        const SModelMaterialRecord *materials = nullptr;
        const SModelTextureRecord *textures = nullptr;

        // Node graph (V2)
        const SModelNodeRecord *nodes = nullptr;
        const uint32_t *nodePrimitiveIndices = nullptr;

        // String table start pointer (C-string table)
        const char *stringTable = nullptr;

        // Blob start pointer (vertex/index/image bytes)
        const uint8_t *blob = nullptr;

        // Small helpers
        uint32_t meshCount() const { return header ? header->meshCount : 0; }
        uint32_t primitiveCount() const { return header ? header->primitiveCount : 0; }
        uint32_t materialCount() const { return header ? header->materialCount : 0; }
        uint32_t textureCount() const { return header ? header->textureCount : 0; }
        uint32_t nodeCount() const { return header ? header->nodeCount : 0; }
        uint32_t nodePrimitiveIndexCount() const { return header ? header->nodePrimitiveIndexCount : 0; }

        // Returns pointer to a null-terminated string in the string table.
        // Returns empty string if offset is 0 or invalid.
        const char *getStringOrEmpty(uint32_t strOffset) const;
    };

    // ------------------------------------------------------------
    // LoadSModelFile
    // ------------------------------------------------------------
    // Loads and validates a cooked .smodel file from disk.
    //
    // Returns true on success.
    // If it fails, outError will contain a short reason.
    bool LoadSModelFile(const std::string &path, SModelFileView &outView, std::string &outError);

}
