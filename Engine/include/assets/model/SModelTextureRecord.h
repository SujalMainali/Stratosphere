#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // Texture Record (V1)
    // ============================================================
    // Stores:
    // - sampler parameters (wrap/filter)
    // - color space (sRGB/Linear)
    // - embedded compressed image bytes (PNG/JPG) in the blob section
    //
    // Runtime will decode bytes to RGBA8 and upload to VkImage.
    struct SModelTextureRecord
    {
        // Offset into string table (0 = none)
        uint32_t nameStrOffset; // debug / friendly name
        uint32_t uriStrOffset;  // original source path/uri (optional)

        uint32_t colorSpace; // TextureColorSpace
        uint32_t encoding;   // ImageEncoding (PNG/JPG/RAW)

        // Sampler settings (mapped later to VkSampler)
        uint32_t wrapU;     // WrapMode
        uint32_t wrapV;     // WrapMode
        uint32_t minFilter; // FilterMode
        uint32_t magFilter; // FilterMode
        uint32_t mipFilter; // MipMode

        float maxAnisotropy; // 1.0 = disabled, >1.0 enable anisotropy

        // Embedded bytes stored in the blob section (relative offsets)
        uint64_t imageDataOffset; // start of PNG/JPG bytes
        uint64_t imageDataSize;   // compressed image byte size

        uint32_t reserved0;
        uint32_t reserved1;
    };

#pragma pack(pop)

    static_assert(sizeof(SModelTextureRecord) == 64, "SModelTextureRecord size mismatch");

} // namespace Engine::smodel