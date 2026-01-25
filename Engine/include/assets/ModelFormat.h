#pragma once

// ============================================================
// Central include for .smodel v2 binary format structs
// ============================================================
// You can include this one file from both:
// - the cook tool (GltfToSmodel)
// - runtime loader (SModelLoader)
// - AssetManager
//
// This keeps the format definition identical across build/runtime.
// ============================================================

#include "assets/model/SModelEnums.h"
#include "assets/model/SModelHeader.h"
#include "assets/model/SModelMeshRecord.h"
#include "assets/model/SModelPrimitiveRecord.h"
#include "assets/model/SModelTextureRecord.h"
#include "assets/model/SModelMaterialRecord.h"
#include "assets/model/SModelNodeRecord.h"
namespace Engine::smodel
{
    // 'SMOD' little-endian magic
    static constexpr uint32_t SMODEL_MAGIC = 0x444F4D53;

    static constexpr uint16_t SMODEL_VERSION_MAJOR = 2;
    static constexpr uint16_t SMODEL_VERSION_MINOR = 0;

    // Small helper for loader validation.
    // If this returns false, loader should reject the file.
    inline bool isHeaderCompatible(const smodel::SModelHeader &h)
    {
        if (h.magic != SMODEL_MAGIC)
            return false;
        if (h.versionMajor != SMODEL_VERSION_MAJOR)
            return false;

        // Minor might be forward-compatible in future.
        return true;
    }

} // namespace Engine::smodel
