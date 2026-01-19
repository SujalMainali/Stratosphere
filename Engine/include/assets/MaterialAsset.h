#pragma once
#include <cstdint>
#include "assets/Handles.h" // TextureHandle

namespace Engine
{
    // ------------------------------------------------------------
    // MaterialAsset (CPU-only)
    // ------------------------------------------------------------
    // Stores glTF metallic-roughness PBR parameters + texture handles.
    // Rendering will later bind textures + push factors.
    struct MaterialAsset
    {
        // PBR factors
        float baseColorFactor[4] = {1, 1, 1, 1};
        float emissiveFactor[3] = {0, 0, 0};
        float metallicFactor = 1.0f;
        float roughnessFactor = 1.0f;

        float normalScale = 1.0f;
        float occlusionStrength = 1.0f;
        float alphaCutoff = 0.5f;

        uint32_t alphaMode = 0;   // 0=Opaque,1=Mask,2=Blend
        uint32_t doubleSided = 0; // 0/1

        // Texture bindings (- invalid handle means "none")
        TextureHandle baseColorTexture{};
        TextureHandle normalTexture{};
        TextureHandle metallicRoughnessTexture{};
        TextureHandle occlusionTexture{};
        TextureHandle emissiveTexture{};

        // TexCoord set selection (usually 0)
        uint32_t baseColorTexCoord = 0;
        uint32_t normalTexCoord = 0;
        uint32_t metallicRoughnessTexCoord = 0;
        uint32_t occlusionTexCoord = 0;
        uint32_t emissiveTexCoord = 0;

        // Debug name (optional)
        const char *debugName = "";
    };

} // namespace Engine
