#include "assets/TextureAsset.h"
#include "utils/ImageUtils.h"

#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

// ------------------------------------------------------------
// stb_image decoding (PNG/JPG)
// ------------------------------------------------------------
#define STB_IMAGE_IMPLEMENTATION
#include "ThirdParty/Stb/stb_image.h"

namespace Engine
{
    static uint32_t calcMipLevels(uint32_t width, uint32_t height)
    {
        const uint32_t maxDim = std::max(width, height);
        if (maxDim == 0)
            return 1;
        return static_cast<uint32_t>(std::floor(std::log2(static_cast<double>(maxDim)))) + 1u;
    }

    bool TextureAsset::uploadRGBA8_Deferred(
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
        float maxAnisotropy)
    {
        if (!ctx.begun || ctx.cmd == VK_NULL_HANDLE)
            return false;

        if (!rgbaPixels || width == 0 || height == 0)
            return false;

        // If already valid, destroy old GPU resources first
        if (isValid())
            destroy(ctx.device);

        m_width = width;
        m_height = height;
        m_mipLevels = calcMipLevels(width, height);
        m_format = srgbFormat ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;

        const VkDeviceSize pixelBytes = VkDeviceSize(width) * VkDeviceSize(height) * 4u;

        // 1) Create staging buffer (must survive until submit finishes)
        StagingBufferHandle staging{};
        VkResult r = CreateStagingBuffer(ctx.device, ctx.physicalDevice, rgbaPixels, pixelBytes, staging);
        if (r != VK_SUCCESS)
            return false;

        // Keep staging alive until EndSubmitAndWait
        ctx.pendingStaging.push_back(staging);

        // 2) Create GPU image (with mip levels)
        r = CreateImage2D(
            ctx.device, ctx.physicalDevice,
            width, height,
            m_format,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            m_mipLevels,
            m_image, m_memory);

        if (r != VK_SUCCESS)
            return false;

        // 3) Record transitions + copy (NO submit here)
        CmdTransitionImageLayout(
            ctx,
            m_image,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_ASPECT_COLOR_BIT);

        CmdCopyBufferToImage(ctx, staging.buffer, m_image, width, height);

        // 3b) Generate mipmaps if possible; otherwise just transition mip 0.
        if (m_mipLevels > 1)
        {
            const bool okMips = CmdGenerateMipmaps(
                ctx,
                m_image,
                m_format,
                static_cast<int32_t>(width),
                static_cast<int32_t>(height),
                m_mipLevels);

            if (!okMips)
            {
                // Fall back to no mips.
                m_mipLevels = 1;
                CmdTransitionImageLayout(
                    ctx,
                    m_image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }
        else
        {
            CmdTransitionImageLayout(
                ctx,
                m_image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_ASPECT_COLOR_BIT);
        }

        // 4) Create view now (safe)
        r = CreateImageView2D(ctx.device, m_image, m_format, VK_IMAGE_ASPECT_COLOR_BIT, m_mipLevels, m_view);
        if (r != VK_SUCCESS)
            return false;

        // 5) Create sampler now (per texture for now)
        r = CreateTextureSampler(
            ctx.device,
            ctx.physicalDevice,
            wrapU,
            wrapV,
            minFilter,
            magFilter,
            mipMode,
            maxAnisotropy,
            static_cast<float>((m_mipLevels > 0) ? (m_mipLevels - 1) : 0),
            m_sampler);

        if (r != VK_SUCCESS)
            return false;

        return true;
    }

    bool TextureAsset::uploadEncodedImage_Deferred(
        UploadContext &ctx,
        const uint8_t *encodedBytes,
        size_t encodedSize,
        bool srgbFormat,
        VkSamplerAddressMode wrapU,
        VkSamplerAddressMode wrapV,
        VkFilter minFilter,
        VkFilter magFilter,
        VkSamplerMipmapMode mipMode,
        float maxAnisotropy)
    {
        if (!encodedBytes || encodedSize == 0)
            return false;

        // Decode PNG/JPG to RGBA8 (4 channels)
        int w = 0, h = 0, comp = 0;
        unsigned char *decoded = stbi_load_from_memory(
            encodedBytes,
            static_cast<int>(encodedSize),
            &w, &h,
            &comp,
            4);

        if (!decoded || w <= 0 || h <= 0)
        {
            if (decoded)
                stbi_image_free(decoded);
            return false;
        }

        // Record GPU upload for decoded pixels
        const bool ok = uploadRGBA8_Deferred(
            ctx,
            decoded,
            static_cast<uint32_t>(w),
            static_cast<uint32_t>(h),
            srgbFormat,
            wrapU,
            wrapV,
            minFilter,
            magFilter,
            mipMode,
            maxAnisotropy);

        stbi_image_free(decoded);
        return ok;
    }

    void TextureAsset::destroy(VkDevice device)
    {
        if (m_sampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, m_sampler, nullptr);
            m_sampler = VK_NULL_HANDLE;
        }

        if (m_view != VK_NULL_HANDLE)
        {
            vkDestroyImageView(device, m_view, nullptr);
            m_view = VK_NULL_HANDLE;
        }

        if (m_image != VK_NULL_HANDLE)
        {
            vkDestroyImage(device, m_image, nullptr);
            m_image = VK_NULL_HANDLE;
        }

        if (m_memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, m_memory, nullptr);
            m_memory = VK_NULL_HANDLE;
        }

        m_width = 0;
        m_height = 0;
        m_mipLevels = 1;
        m_format = VK_FORMAT_R8G8B8A8_UNORM;
    }

} // namespace Engine
