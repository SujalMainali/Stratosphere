#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // Material Record (V1)
    // ============================================================
    // glTF Metallic-Roughness PBR base.
    //
    // Texture indices reference SModelTextureRecordV1 entries.
    // -1 means the texture is not present.
    struct SModelMaterialRecord
    {
        uint32_t nameStrOffset; // optional name for debugging

        // PBR factors (defaults match glTF defaults)
        float baseColorFactor[4]; // default 1,1,1,1
        float emissiveFactor[3];  // default 0,0,0

        float metallicFactor;  // default 1
        float roughnessFactor; // default 1

        float normalScale;       // default 1
        float occlusionStrength; // default 1
        float alphaCutoff;       // default 0.5

        uint32_t alphaMode;   // AlphaMode
        uint32_t doubleSided; // 0/1

        // Texture indices into texture table (-1 means "none")
        int32_t baseColorTexture;         // sRGB
        int32_t normalTexture;            // Linear
        int32_t metallicRoughnessTexture; // Linear
        int32_t occlusionTexture;         // Linear
        int32_t emissiveTexture;          // sRGB

        // glTF allows choosing which UV set a texture uses (usually 0)
        uint32_t baseColorTexCoord;
        uint32_t normalTexCoord;
        uint32_t metallicRoughnessTexCoord;
        uint32_t occlusionTexCoord;
        uint32_t emissiveTexCoord;

        uint32_t reserved;
    };

#pragma pack(pop)

    static_assert(sizeof(SModelMaterialRecord) == 104, "SModelMaterialRecord size mismatch");

} // namespace Engine::smodel
