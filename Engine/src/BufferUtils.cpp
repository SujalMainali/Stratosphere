#include "utils/BufferUtils.h"
#include <stdexcept>
#include <cstring>

namespace Engine
{

    static uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags properties)
    {
        // This function finds a suitable memory type based on the type filter and required properties
        // The index of the required memory in the GPU is returned
        VkPhysicalDeviceMemoryProperties memProps{};
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
        {
            if ((typeFilter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & properties) == properties)
            {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type");
    }

    VkResult CreateOrUpdateVertexBuffer(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        const void *vertexData,
        VkDeviceSize dataSize,
        VertexBufferHandle &handle)
    {
        // This function creates or updates a vertex buffer with the provided vertex data.
        // The final buffer is returned in the handle.buffer
        if (!vertexData || dataSize == 0)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        // If buffer exists, check current size by querying memory requirements
        bool needCreate = (handle.buffer == VK_NULL_HANDLE);
        VkMemoryRequirements req{};
        if (!needCreate)
        {
            vkGetBufferMemoryRequirements(device, handle.buffer, &req);
            if (req.size < dataSize)
            {
                // Destroy and recreate with larger size
                vkDestroyBuffer(device, handle.buffer, nullptr);
                handle.buffer = VK_NULL_HANDLE;
                vkFreeMemory(device, handle.memory, nullptr);
                handle.memory = VK_NULL_HANDLE;
                needCreate = true;
            }
        }

        if (needCreate)
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = dataSize;
            bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkBuffer buffer = VK_NULL_HANDLE;
            VkResult r = vkCreateBuffer(device, &bi, nullptr, &buffer);
            if (r != VK_SUCCESS)
                return r;

            // This returns the memory requirements as specified by the usage and size of the buffer
            vkGetBufferMemoryRequirements(device, buffer, &req);

            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            VkDeviceMemory mem = VK_NULL_HANDLE;
            r = vkAllocateMemory(device, &ai, nullptr, &mem);
            if (r != VK_SUCCESS)
            {
                vkDestroyBuffer(device, buffer, nullptr);
                return r;
            }

            r = vkBindBufferMemory(device, buffer, mem, 0);
            if (r != VK_SUCCESS)
            {
                vkDestroyBuffer(device, buffer, nullptr);
                vkFreeMemory(device, mem, nullptr);
                return r;
            }

            handle.buffer = buffer;
            handle.memory = mem;
        }

        // Map and copy
        void *mapped = nullptr;
        VkResult r = vkMapMemory(device, handle.memory, 0, dataSize, 0, &mapped); // This creates a CPU accessible pointer i.e. mapped to the GPU memory
        if (r != VK_SUCCESS)
            return r;
        std::memcpy(mapped, vertexData, static_cast<size_t>(dataSize)); // This copies the vertex data into the GPU memory pointed by mapped
        vkUnmapMemory(device, handle.memory);

        return VK_SUCCESS;
    }

    void DestroyVertexBuffer(VkDevice device, VertexBufferHandle &handle)
    {
        if (handle.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, handle.buffer, nullptr);
            handle.buffer = VK_NULL_HANDLE;
        }
        if (handle.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, handle.memory, nullptr);
            handle.memory = VK_NULL_HANDLE;
        }
    }

    VkResult CreateOrUpdateIndexBuffer(
        VkDevice device,
        VkPhysicalDevice physicalDevice,
        const void *indexData,
        VkDeviceSize dataSize,
        IndexBufferHandle &handle)
    {
        if (!indexData || dataSize == 0)
        {
            return VK_ERROR_INITIALIZATION_FAILED;
        }

        bool needCreate = (handle.buffer == VK_NULL_HANDLE);
        VkMemoryRequirements req{};
        if (!needCreate)
        {
            vkGetBufferMemoryRequirements(device, handle.buffer, &req);
            if (req.size < dataSize)
            {
                vkDestroyBuffer(device, handle.buffer, nullptr);
                handle.buffer = VK_NULL_HANDLE;
                vkFreeMemory(device, handle.memory, nullptr);
                handle.memory = VK_NULL_HANDLE;
                needCreate = true;
            }
        }

        if (needCreate)
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = dataSize;
            bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            VkBuffer buffer = VK_NULL_HANDLE;
            VkResult r = vkCreateBuffer(device, &bi, nullptr, &buffer);
            if (r != VK_SUCCESS)
                return r;

            vkGetBufferMemoryRequirements(device, buffer, &req);

            VkMemoryAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = findMemoryType(physicalDevice, req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

            VkDeviceMemory mem = VK_NULL_HANDLE;
            r = vkAllocateMemory(device, &ai, nullptr, &mem);
            if (r != VK_SUCCESS)
            {
                vkDestroyBuffer(device, buffer, nullptr);
                return r;
            }

            r = vkBindBufferMemory(device, buffer, mem, 0);
            if (r != VK_SUCCESS)
            {
                vkDestroyBuffer(device, buffer, nullptr);
                vkFreeMemory(device, mem, nullptr);
                return r;
            }

            handle.buffer = buffer;
            handle.memory = mem;
        }

        void *mapped = nullptr;
        VkResult r = vkMapMemory(device, handle.memory, 0, dataSize, 0, &mapped);
        if (r != VK_SUCCESS)
            return r;
        std::memcpy(mapped, indexData, static_cast<size_t>(dataSize));
        vkUnmapMemory(device, handle.memory);

        return VK_SUCCESS;
    }

    void DestroyIndexBuffer(VkDevice device, IndexBufferHandle &handle)
    {
        if (handle.buffer != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, handle.buffer, nullptr);
            handle.buffer = VK_NULL_HANDLE;
        }
        if (handle.memory != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, handle.memory, nullptr);
            handle.memory = VK_NULL_HANDLE;
        }
    }

} // namespace Engine