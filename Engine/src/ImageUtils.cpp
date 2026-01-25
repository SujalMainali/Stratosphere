#include "utils/ImageUtils.h"
#include <cstring>
#include <stdexcept>

namespace Engine
{
    // ============================================================
    // Memory helper
    // ============================================================

    static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags props)
    {
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeBits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
                return i;
        }

        throw std::runtime_error("ImageUtils: failed to find suitable memory type");
    }

    // ============================================================
    // Staging buffer
    // ============================================================

    VkResult CreateStagingBuffer(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        const void *dataBytes,
        VkDeviceSize dataSize,
        StagingBufferHandle &out)
    {
        if (!dataBytes || dataSize == 0)
            return VK_ERROR_INITIALIZATION_FAILED;

        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = dataSize;
        bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateBuffer(device, &bi, nullptr, &out.buffer);
        if (r != VK_SUCCESS)
            return r;

        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, out.buffer, &req);

        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(
            physicalDevice, req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

        r = vkAllocateMemory(device, &ai, nullptr, &out.memory);
        if (r != VK_SUCCESS)
        {
            vkDestroyBuffer(device, out.buffer, nullptr);
            out.buffer = VK_NULL_HANDLE;
            return r;
        }

        r = vkBindBufferMemory(device, out.buffer, out.memory, 0);
        if (r != VK_SUCCESS)
        {
            vkDestroyBuffer(device, out.buffer, nullptr);
            vkFreeMemory(device, out.memory, nullptr);
            out.buffer = VK_NULL_HANDLE;
            out.memory = VK_NULL_HANDLE;
            return r;
        }

        void *mapped = nullptr;
        r = vkMapMemory(device, out.memory, 0, dataSize, 0, &mapped);
        if (r != VK_SUCCESS)
            return r;

        std::memcpy(mapped, dataBytes, static_cast<size_t>(dataSize));
        vkUnmapMemory(device, out.memory);

        out.size = dataSize;
        return VK_SUCCESS;
    }

    void DestroyStagingBuffer(VkDevice device, StagingBufferHandle &h)
    {
        if (h.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, h.buffer, nullptr);
            h.buffer = VK_NULL_HANDLE;
        }
        if (h.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, h.memory, nullptr);
            h.memory = VK_NULL_HANDLE;
        }
        h.size = 0;
    }

    // ============================================================
    // UploadContext
    // ============================================================

    bool BeginUploadContext(
        UploadContext &ctx,
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        VkCommandPool commandPool,
        VkQueue queue)
    {
        ctx.device = device;
        ctx.physicalDevice = physicalDevice;
        ctx.commandPool = commandPool;
        ctx.queue = queue;
        ctx.pendingStaging.clear();
        ctx.begun = false;

        // Allocate one primary command buffer
        VkCommandBufferAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc.commandPool = commandPool;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = 1;

        VkResult r = vkAllocateCommandBuffers(device, &alloc, &ctx.cmd);
        if (r != VK_SUCCESS)
            return false;

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        r = vkBeginCommandBuffer(ctx.cmd, &begin);
        if (r != VK_SUCCESS)
        {
            vkFreeCommandBuffers(device, commandPool, 1, &ctx.cmd);
            ctx.cmd = VK_NULL_HANDLE;
            return false;
        }

        ctx.begun = true;
        return true;
    }

    bool EndSubmitAndWait(UploadContext &ctx)
    {
        if (!ctx.begun || ctx.cmd == VK_NULL_HANDLE)
            return false;

        VkResult r = vkEndCommandBuffer(ctx.cmd);
        if (r != VK_SUCCESS)
            return false;

        // Fence so we can wait for completion (better than queueWaitIdle spam)
        VkFence fence = VK_NULL_HANDLE;
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        r = vkCreateFence(ctx.device, &fi, nullptr, &fence);
        if (r != VK_SUCCESS)
            return false;

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &ctx.cmd;

        r = vkQueueSubmit(ctx.queue, 1, &submit, fence);
        if (r != VK_SUCCESS)
        {
            vkDestroyFence(ctx.device, fence, nullptr);
            return false;
        }

        // Wait once for the whole model upload
        r = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
        vkDestroyFence(ctx.device, fence, nullptr);

        if (r != VK_SUCCESS)
            return false;

        // Now safe to destroy all staging buffers
        for (auto &sb : ctx.pendingStaging)
            DestroyStagingBuffer(ctx.device, sb);
        ctx.pendingStaging.clear();

        // Free command buffer
        vkFreeCommandBuffers(ctx.device, ctx.commandPool, 1, &ctx.cmd);
        ctx.cmd = VK_NULL_HANDLE;
        ctx.begun = false;

        return true;
    }

    // ============================================================
    // Image creation
    // ============================================================

    VkResult CreateImage2D(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImage &outImage,
        VkDeviceMemory &outMemory)
    {
        return CreateImage2D(device, physicalDevice, width, height, format, usage, 1u, outImage, outMemory);
    }

    VkResult CreateImage2D(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        uint32_t width,
        uint32_t height,
        VkFormat format,
        VkImageUsageFlags usage,
        uint32_t mipLevels,
        VkImage &outImage,
        VkDeviceMemory &outMemory)
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.extent.width = width;
        ii.extent.height = height;
        ii.extent.depth = 1;
        ii.mipLevels = (mipLevels == 0) ? 1u : mipLevels;
        ii.arrayLayers = 1;
        ii.format = format;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ii.usage = usage;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult r = vkCreateImage(device, &ii, nullptr, &outImage);
        if (r != VK_SUCCESS)
            return r;

        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device, outImage, &req);

        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        r = vkAllocateMemory(device, &ai, nullptr, &outMemory);
        if (r != VK_SUCCESS)
        {
            vkDestroyImage(device, outImage, nullptr);
            outImage = VK_NULL_HANDLE;
            return r;
        }

        r = vkBindImageMemory(device, outImage, outMemory, 0);
        if (r != VK_SUCCESS)
        {
            vkDestroyImage(device, outImage, nullptr);
            vkFreeMemory(device, outMemory, nullptr);
            outImage = VK_NULL_HANDLE;
            outMemory = VK_NULL_HANDLE;
            return r;
        }

        return VK_SUCCESS;
    }

    VkResult CreateImageView2D(
        VkDevice device,
        VkImage image,
        VkFormat format,
        VkImageAspectFlags aspectFlags,
        VkImageView &outView)
    {
        return CreateImageView2D(device, image, format, aspectFlags, 1u, outView);
    }

    VkResult CreateImageView2D(
        VkDevice device,
        VkImage image,
        VkFormat format,
        VkImageAspectFlags aspectFlags,
        uint32_t mipLevels,
        VkImageView &outView)
    {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;

        vi.subresourceRange.aspectMask = aspectFlags;
        vi.subresourceRange.baseMipLevel = 0;
        vi.subresourceRange.levelCount = (mipLevels == 0) ? 1u : mipLevels;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount = 1;

        return vkCreateImageView(device, &vi, nullptr, &outView);
    }

    // ============================================================
    // Command recording helpers (optimized)
    // ============================================================

    // Minimal barrier config to support our common transitions.
    static void fillBarrierMasks(
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkAccessFlags &outSrcAccess,
        VkAccessFlags &outDstAccess,
        VkPipelineStageFlags &outSrcStage,
        VkPipelineStageFlags &outDstStage)
    {
        outSrcAccess = 0;
        outDstAccess = 0;
        outSrcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        outDstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
            newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
        {
            outSrcAccess = 0;
            outDstAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            outSrcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            outDstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        }
        else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
                 newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            outSrcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
            outDstAccess = VK_ACCESS_SHADER_READ_BIT;
            outSrcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            outDstStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        }
        else
        {
            // Fallback conservative barrier
            outSrcAccess = 0;
            outDstAccess = 0;
            outSrcStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            outDstStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        }
    }

    void CmdTransitionImageLayout(
        UploadContext &ctx,
        VkImage image,
        VkImageLayout oldLayout,
        VkImageLayout newLayout,
        VkImageAspectFlags aspectFlags)
    {
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;

        barrier.subresourceRange.aspectMask = aspectFlags;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags srcStage, dstStage;
        VkAccessFlags srcAccess, dstAccess;
        fillBarrierMasks(oldLayout, newLayout, srcAccess, dstAccess, srcStage, dstStage);

        barrier.srcAccessMask = srcAccess;
        barrier.dstAccessMask = dstAccess;

        vkCmdPipelineBarrier(
            ctx.cmd,
            srcStage,
            dstStage,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);
    }

    void CmdCopyBufferToImage(
        UploadContext &ctx,
        VkBuffer buffer,
        VkImage image,
        uint32_t width,
        uint32_t height)
    {
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;

        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;

        region.imageOffset = {0, 0, 0};
        region.imageExtent = {width, height, 1};

        vkCmdCopyBufferToImage(
            ctx.cmd,
            buffer,
            image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &region);
    }

    bool CmdGenerateMipmaps(
        UploadContext &ctx,
        VkImage image,
        VkFormat format,
        int32_t width,
        int32_t height,
        uint32_t mipLevels)
    {
        if (!ctx.begun || ctx.cmd == VK_NULL_HANDLE)
            return false;
        if (mipLevels <= 1)
            return true;

        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice, format, &props);

        const bool supportsLinearBlit =
            (props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        if (!supportsLinearBlit)
            return false;

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.image = image;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
        barrier.subresourceRange.levelCount = 1;

        int32_t mipW = width;
        int32_t mipH = height;

        for (uint32_t i = 1; i < mipLevels; ++i)
        {
            // Transition (i-1) to TRANSFER_SRC for blit
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            vkCmdPipelineBarrier(
                ctx.cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier);

            // Transition mip i to TRANSFER_DST
            barrier.subresourceRange.baseMipLevel = i;
            barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(
                ctx.cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier);

            VkImageBlit blit{};
            blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = 1;
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {mipW, mipH, 1};

            const int32_t nextW = (mipW > 1) ? (mipW / 2) : 1;
            const int32_t nextH = (mipH > 1) ? (mipH / 2) : 1;

            blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = 1;
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {nextW, nextH, 1};

            vkCmdBlitImage(
                ctx.cmd,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &blit,
                VK_FILTER_LINEAR);

            // Transition (i-1) to SHADER_READ_ONLY
            barrier.subresourceRange.baseMipLevel = i - 1;
            barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(
                ctx.cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0,
                0, nullptr,
                0, nullptr,
                1, &barrier);

            mipW = nextW;
            mipH = nextH;
        }

        // Transition last mip to SHADER_READ_ONLY
        barrier.subresourceRange.baseMipLevel = mipLevels - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            ctx.cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);

        return true;
    }

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
        VkSampler &outSampler)
    {
        return CreateTextureSampler(device, physicalDevice, addressU, addressV, minFilter, magFilter, mipMode, maxAnisotropy, 0.0f, outSampler);
    }

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
        VkSampler &outSampler)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physicalDevice, &props);

        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;

        si.addressModeU = addressU;
        si.addressModeV = addressV;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;

        si.minFilter = minFilter;
        si.magFilter = magFilter;
        si.mipmapMode = mipMode;

        si.minLod = 0.0f;
        si.maxLod = (maxLod > 0.0f) ? maxLod : 0.0f;
        si.mipLodBias = 0.0f;

        // Optional anisotropy (if supported and requested)
        if (maxAnisotropy > 1.0f && props.limits.maxSamplerAnisotropy > 1.0f)
        {
            si.anisotropyEnable = VK_TRUE;
            si.maxAnisotropy = (maxAnisotropy > props.limits.maxSamplerAnisotropy)
                                   ? props.limits.maxSamplerAnisotropy
                                   : maxAnisotropy;
        }
        else
        {
            si.anisotropyEnable = VK_FALSE;
            si.maxAnisotropy = 1.0f;
        }

        si.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        si.unnormalizedCoordinates = VK_FALSE;

        si.compareEnable = VK_FALSE;
        si.compareOp = VK_COMPARE_OP_ALWAYS;

        return vkCreateSampler(device, &si, nullptr, &outSampler);
    }

} // namespace Engine
