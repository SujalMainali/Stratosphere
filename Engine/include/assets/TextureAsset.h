#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>

namespace Engine
{
    struct UploadContext; // forward decl from ImageUtils

    // ============================================================
    // TextureAsset
    // ============================================================
    // Owns GPU objects:
    // - VkImage + memory
    // - VkImageView
    // - VkSampler  (per texture for now)
    class TextureAsset
    {
    public:
        TextureAsset() = default;
        ~TextureAsset() = default;

        // ------------------------------------------------------------
        // Optimized upload path (records commands, NO submit)
        // ------------------------------------------------------------
        bool uploadRGBA8_Deferred(
            UploadContext &ctx,
            const uint8_t *rgbaPixels,
            uint32_t width,
            uint32_t height,
            bool srgbFormat,
            VkSamplerAddressMode wrapU,
            VkSamplerAddressMode wrapV,
            VkFilter minFilter,
            VkFilter magFilter,
            VkSamplerMipmapMode mipMode,
            float maxAnisotropy);

        bool uploadEncodedImage_Deferred(
            UploadContext &ctx,
            const uint8_t *encodedBytes,
            size_t encodedSize,
            bool srgbFormat,
            VkSamplerAddressMode wrapU,
            VkSamplerAddressMode wrapV,
            VkFilter minFilter,
            VkFilter magFilter,
            VkSamplerMipmapMode mipMode,
            float maxAnisotropy);

        // Destroy GPU resources (used by AssetManager when freeing)
        void destroy(VkDevice device);

        // Accessors
        VkImage getImage() const { return m_image; }
        VkImageView getView() const { return m_view; }
        VkSampler getSampler() const { return m_sampler; }
        uint32_t getWidth() const { return m_width; }
        uint32_t getHeight() const { return m_height; }
        uint32_t getMipLevels() const { return m_mipLevels; }
        VkFormat getFormat() const { return m_format; }

        bool isValid() const { return m_image != VK_NULL_HANDLE; }

    private:
        VkImage m_image = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        VkSampler m_sampler = VK_NULL_HANDLE;

        uint32_t m_width = 0;
        uint32_t m_height = 0;
        uint32_t m_mipLevels = 1;
        VkFormat m_format = VK_FORMAT_R8G8B8A8_UNORM;
    };

} // namespace Engine
