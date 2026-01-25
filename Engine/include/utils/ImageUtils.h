#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>

namespace Engine
{
    // ============================================================
    // Staging buffer handle
    // ============================================================
    // Used to upload pixel bytes to GPU images.
    // This is CPU-visible memory.
    struct StagingBufferHandle
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
    };

    // Creates a host-visible staging buffer and fills it with the provided bytes.
    VkResult CreateStagingBuffer(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        const void *dataBytes,
        VkDeviceSize dataSize,
        StagingBufferHandle &out);

    // Destroy staging buffer resources.
    void DestroyStagingBuffer(VkDevice device, StagingBufferHandle &h);

    // ============================================================
    // UploadContext (optimized path)
    // ============================================================
    // This records *many* texture uploads into ONE command buffer and submits once.
    //
    // Typical usage:
    //   UploadContext ctx;
    //   BeginUploadContext(ctx, device, phys, pool, queue);
    //   ... record transitions + buffer copies for multiple textures ...
    //   EndSubmitAndWait(ctx);
    //
    struct UploadContext
    {
        VkDevice device = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;

        VkCommandBuffer cmd = VK_NULL_HANDLE;

        // Staging buffers must stay alive until GPU copy finishes.
        // We'll collect them here and destroy at the end.
        std::vector<StagingBufferHandle> pendingStaging;

        bool begun = false;
    };

    bool BeginUploadContext(
        UploadContext &ctx,
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkCommandPool commandPool,
        VkQueue queue);

    // Submits command buffer, waits for completion, destroys staging buffers, frees cmd buffer.
    bool EndSubmitAndWait(UploadContext &ctx);

    // ============================================================
    // Image creation helpers
    // ============================================================

    // Create a 2D GPU image (device local).
    // Phase 1: mipLevels = 1 (mipmaps later).
    VkResult CreateImage2D(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImage &outImage,
        VkDeviceMemory &outMemory);

    // Create a 2D GPU image with explicit mip levels.
    VkResult CreateImage2D(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        uint32_t mipLevels,
        VkImage &outImage,
        VkDeviceMemory &outMemory);

    // Create an image view for sampling.
    VkResult CreateImageView2D(
        VkDevice device,
        VkImage image,
        VkFormat format,
        VkImageAspectFlags aspectFlags,
        VkImageView &outView);

    // Create an image view spanning multiple mip levels.
    VkResult CreateImageView2D(
        VkDevice device,
        VkImage image,
        VkFormat format,
        VkImageAspectFlags aspectFlags,
        uint32_t mipLevels,
        VkImageView &outView);

    // ============================================================
    // Command recording helpers (NO submit here!)
    // ============================================================
    // These functions RECORD commands into ctx.cmd.
    // They do not submit / wait. That is handled in EndSubmitAndWait().

    void CmdTransitionImageLayout(
        UploadContext &ctx,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkImageAspectFlags aspectFlags);

    void CmdCopyBufferToImage(
        UploadContext &ctx,
        VkBuffer buffer,
        VkImage image,
        uint32_t width,
        uint32_t height);

    // Record mipmap generation via blits for a 2D color image.
    // Expects mip 0 to be in TRANSFER_DST_OPTIMAL.
    // On success, transitions all mips to SHADER_READ_ONLY_OPTIMAL.
    bool CmdGenerateMipmaps(
        UploadContext &ctx,
        VkImage image,
        VkFormat format,
        int32_t width,
        int32_t height,
        uint32_t mipLevels);

    // ============================================================
    // Sampler creation
    // ============================================================
    VkResult CreateTextureSampler(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkSamplerAddressMode addressU,
        VkSamplerAddressMode addressV,
        VkFilter minFilter,
        VkFilter magFilter,
        VkSamplerMipmapMode mipMode,
        float maxAnisotropy,
        VkSampler &outSampler);

    VkResult CreateTextureSampler(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkSamplerAddressMode addressU,
        VkSamplerAddressMode addressV,
        VkFilter minFilter,
        VkFilter magFilter,
        VkSamplerMipmapMode mipMode,
        float maxAnisotropy,
        float maxLod,
        VkSampler &outSampler);

} // namespace Engine
