#include <stdexcept>
#include <iostream>
#include <cstring>
#include <chrono>
#include "Engine/Renderer.h"
#include "Engine/VulkanContext.h"
#include "Engine/SwapChain.h"
#include "utils/ImageUtils.h"

namespace Engine
{
    static VkFormat findSupportedFormat(
        VkPhysicalDevice phys,
        const std::vector<VkFormat> &candidates,
        VkImageTiling tiling,
        VkFormatFeatureFlags features)
    {
        for (VkFormat format : candidates)
        {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(phys, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features)
                return format;
            if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features)
                return format;
        }
        return VK_FORMAT_UNDEFINED;
    }

    static VkFormat findDepthFormat(VkPhysicalDevice phys)
    {
        // Prefer D32; fall back to common packed depth/stencil formats.
        return findSupportedFormat(
            phys,
            {
                VK_FORMAT_D32_SFLOAT,
                VK_FORMAT_D32_SFLOAT_S8_UINT,
                VK_FORMAT_D24_UNORM_S8_UINT,
            },
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
    }

    static VkImageAspectFlags depthAspectFlags(VkFormat fmt)
    {
        VkImageAspectFlags flags = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (fmt == VK_FORMAT_D32_SFLOAT_S8_UINT || fmt == VK_FORMAT_D24_UNORM_S8_UINT)
            flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return flags;
    }

    Renderer::Renderer(VulkanContext *ctx, SwapChain *swapchain, uint32_t maxFramesInFlight)
        : m_ctx(ctx), m_swapchain(swapchain), m_maxFrames(maxFramesInFlight)
    {
        if (!m_ctx || !m_swapchain)
        {
            throw std::runtime_error("Renderer: VulkanContext and Swapchain must be non-null");
        }

        // Extract commonly used handles (adjust if your context uses getters)
        m_device = m_ctx->GetDevice();
        m_graphicsQueue = m_ctx->GetGraphicsQueue();
        m_presentQueue = m_ctx->GetPresentQueue();

        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();
    }

    Renderer::~Renderer()
    {
        // Ensure cleanup was called
        if (m_initialized)
        {
            try
            {
                cleanup();
            }
            catch (...)
            {
                // Avoid throwing from destructor
            }
        }
    }

    void Renderer::init(VkExtent2D extent)
    {
        if (m_initialized)
            return;

        // If a non-zero extent is provided, override the current extent
        if (extent.width > 0 && extent.height > 0)
        {
            m_extent = extent;
        }

        // prepare per-frame slots
        m_frames.resize(m_maxFrames);

        // swapchain-dependent
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();
        createSyncObjects();
        createCommandPoolsAndBuffers();
        createTimestampQueryPool();

        // notify registered passes so they can create pipelines/resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }

        m_initialized = true;
    }

    void Renderer::init() // Overloaded init function to be used without passing extent
    {
        if (m_initialized)
            return;

        // prepare per-frame slots
        m_frames.resize(m_maxFrames);

        // swapchain-dependent
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();
        createSyncObjects();
        createCommandPoolsAndBuffers();
        createTimestampQueryPool();

        // notify registered passes so they can create pipelines/resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }

        m_initialized = true;
    }

    void Renderer::cleanup()
    {
        if (!m_initialized)
            return;

        // Wait for GPU to finish using resources before destroying them
        vkDeviceWaitIdle(m_device);

        // Notify passes to destroy their device-owned resources (pipelines, descriptors, etc.)
        for (auto &p : m_passes)
        {
            if (p)
                p->onDestroy(*m_ctx);
        }

        destroyTimestampQueryPool();
        destroyCommandPoolsAndBuffers();
        destroySyncObjects();

        // Destroy framebuffers
        for (auto fb : m_framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
            {
                vkDestroyFramebuffer(m_device, fb, nullptr);
            }
        }
        m_framebuffers.clear();

        destroyDepthResources();

        // Destroy main render pass
        if (m_mainRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_device, m_mainRenderPass, nullptr);
            m_mainRenderPass = VK_NULL_HANDLE;
        }

        m_initialized = false;
    }

    void Renderer::registerPass(std::shared_ptr<RenderPassModule> pass)
    {
        if (!pass)
            return;
        m_passes.push_back(pass);
        if (m_initialized)
        {
            // Immediately call onCreate so the pass can create pipelines that depend on renderpass/framebuffers.
            pass->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }
    }

    void Renderer::createMainRenderPass()
    {
        if (m_depthFormat == VK_FORMAT_UNDEFINED)
        {
            m_depthFormat = findDepthFormat(m_ctx->GetPhysicalDevice());
        }

        // Color attachment tied to swapchain image format
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = m_swapchainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        // Depth attachment (one image per swapchain image)
        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = m_depthFormat;
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        // Subpass dependency from external -> subpass 0
        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        VkAttachmentDescription attachments[2] = {colorAttachment, depthAttachment};
        rpInfo.attachmentCount = 2;
        rpInfo.pAttachments = attachments;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(m_device, &rpInfo, nullptr, &m_mainRenderPass) != VK_SUCCESS)
        {
            throw std::runtime_error("Renderer::createMainRenderPass - failed to create render pass");
        }
    }

    void Renderer::createFramebuffers()
    {
        const auto &imageViews = m_swapchain->GetImageViews();
        m_framebuffers.resize(imageViews.size());

        if (m_depthImageViews.size() != imageViews.size())
        {
            throw std::runtime_error("Renderer::createFramebuffers - depth resources not initialized or size mismatch");
        }

        for (size_t i = 0; i < imageViews.size(); ++i)
        {
            VkImageView attachments[2] = {imageViews[i], m_depthImageViews[i]};

            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = m_mainRenderPass;
            fbInfo.attachmentCount = 2;
            fbInfo.pAttachments = attachments;
            fbInfo.width = m_extent.width;
            fbInfo.height = m_extent.height;
            fbInfo.layers = 1;

            if (vkCreateFramebuffer(m_device, &fbInfo, nullptr, &m_framebuffers[i]) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createFramebuffers - failed to create framebuffer");
            }
        }
    }

    void Renderer::createDepthResources()
    {
        destroyDepthResources();

        m_depthFormat = findDepthFormat(m_ctx->GetPhysicalDevice());
        if (m_depthFormat == VK_FORMAT_UNDEFINED)
        {
            throw std::runtime_error("Renderer::createDepthResources - failed to find supported depth format");
        }

        const auto &imageViews = m_swapchain->GetImageViews();
        m_depthImages.resize(imageViews.size(), VK_NULL_HANDLE);
        m_depthMemories.resize(imageViews.size(), VK_NULL_HANDLE);
        m_depthImageViews.resize(imageViews.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < imageViews.size(); ++i)
        {
            VkResult r = CreateImage2D(
                m_device,
                m_ctx->GetPhysicalDevice(),
                m_extent.width,
                m_extent.height,
                m_depthFormat,
                VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                m_depthImages[i],
                m_depthMemories[i]);
            if (r != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createDepthResources - failed to create depth image");
            }

            r = CreateImageView2D(
                m_device,
                m_depthImages[i],
                m_depthFormat,
                depthAspectFlags(m_depthFormat),
                m_depthImageViews[i]);
            if (r != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createDepthResources - failed to create depth image view");
            }
        }
    }

    void Renderer::destroyDepthResources()
    {
        for (auto &iv : m_depthImageViews)
        {
            if (iv != VK_NULL_HANDLE)
            {
                vkDestroyImageView(m_device, iv, nullptr);
                iv = VK_NULL_HANDLE;
            }
        }
        for (auto &img : m_depthImages)
        {
            if (img != VK_NULL_HANDLE)
            {
                vkDestroyImage(m_device, img, nullptr);
                img = VK_NULL_HANDLE;
            }
        }
        for (auto &mem : m_depthMemories)
        {
            if (mem != VK_NULL_HANDLE)
            {
                vkFreeMemory(m_device, mem, nullptr);
                mem = VK_NULL_HANDLE;
            }
        }
        m_depthImageViews.clear();
        m_depthImages.clear();
        m_depthMemories.clear();
    }

    void Renderer::recreateSwapchainDependent()
    {
        vkDeviceWaitIdle(m_device);

        // Destroy pass-owned resources that depend on renderpass/framebuffers
        for (auto &p : m_passes)
        {
            if (p)
                p->onDestroy(*m_ctx);
        }

        for (auto fb : m_framebuffers)
        {
            if (fb != VK_NULL_HANDLE)
                vkDestroyFramebuffer(m_device, fb, nullptr);
        }
        m_framebuffers.clear();

        destroyDepthResources();

        if (m_mainRenderPass != VK_NULL_HANDLE)
        {
            vkDestroyRenderPass(m_device, m_mainRenderPass, nullptr);
            m_mainRenderPass = VK_NULL_HANDLE;
        }

        // Swapchain itself has been recreated by caller.
        m_swapchainImageFormat = m_swapchain->GetImageFormat();
        m_extent = m_swapchain->GetExtent();

        createDepthResources();
        createMainRenderPass();
        createFramebuffers();

        for (auto &p : m_passes)
        {
            if (!p)
                continue;
            p->onResize(*m_ctx, m_extent);
            p->onCreate(*m_ctx, m_mainRenderPass, m_framebuffers);
        }
    }

    void Renderer::createSyncObjects()
    {
        // For now we create sync objects per frame in flight to support only single threaded rendering
        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start signaled so the first frame can be submitted immediately

        for (uint32_t i = 0; i < m_maxFrames; ++i)
        {
            FrameContext &f = m_frames[i];

            if (vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &f.imageAcquiredSemaphore) != VK_SUCCESS ||
                vkCreateSemaphore(m_device, &semaphoreInfo, nullptr, &f.renderFinishedSemaphore) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createSyncObjects - failed to create semaphores");
            }

            if (vkCreateFence(m_device, &fenceInfo, nullptr, &f.inFlightFence) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createSyncObjects - failed to create fence");
            }
        }
    }

    void Renderer::createCommandPoolsAndBuffers()
    {
        // For now we create one command pool and buffer per frame in flight to support only single threaded rendering
        for (uint32_t i = 0; i < m_maxFrames; ++i)
        {
            FrameContext &f = m_frames[i];

            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = m_ctx->GetGraphicsQueueFamilyIndex();

            if (vkCreateCommandPool(m_device, &poolInfo, nullptr, &f.commandPool) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createCommandPoolsAndBuffers - failed to create command pool");
            }

            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = f.commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;

            if (vkAllocateCommandBuffers(m_device, &allocInfo, &f.commandBuffer) != VK_SUCCESS)
            {
                throw std::runtime_error("Renderer::createCommandPoolsAndBuffers - failed to allocate command buffer");
            }
        }
    }

    void Renderer::drawFrame()
    {
        if (!m_initialized)
            return;

        using Clock = std::chrono::high_resolution_clock;
        auto msSince = [](Clock::time_point a, Clock::time_point b) -> float
        {
            return std::chrono::duration<float, std::milli>(b - a).count();
        };

        const auto frameStartCpu = Clock::now();
        auto finalizeCpuTotals = [&]()
        {
            m_cpuTimings.drawFrameTotalMs = msSince(frameStartCpu, Clock::now());
            const float accounted =
                m_cpuTimings.waitFenceMs +
                m_cpuTimings.acquireMs +
                m_cpuTimings.cmdResetMs +
                m_cpuTimings.cmdBeginMs +
                m_cpuTimings.timestampResetMs +
                m_cpuTimings.renderPassBeginMs +
                m_cpuTimings.passesRecordMs +
                m_cpuTimings.imguiRecordMs +
                m_cpuTimings.renderPassEndMs +
                m_cpuTimings.cmdEndMs +
                m_cpuTimings.submitMs +
                m_cpuTimings.queryResultsMs +
                m_cpuTimings.presentMs;
            m_cpuTimings.otherMs = m_cpuTimings.drawFrameTotalMs - accounted;
        };

        FrameContext &frame = m_frames[m_currentFrame];
        frame.frameIndex = m_currentFrame;

        // Clear per-pass timings each frame (reused storage).
        if (m_passCpuTimings.size() != m_passes.size())
        {
            m_passCpuTimings.assign(m_passes.size(), PassCpuTiming{});
        }
        else
        {
            for (auto &pt : m_passCpuTimings)
            {
                pt.name = "";
                pt.recordMs = 0.0f;
            }
        }

        // Wait for previous frame to finish
        auto t0 = Clock::now();
        VkResult r = vkWaitForFences(m_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        auto t1 = Clock::now();
        m_cpuTimings.waitFenceMs = msSince(t0, t1);
        if (r != VK_SUCCESS)
        {
            fprintf(stderr, "vkWaitForFences failed: %d\n", r);
            // Recover strategy: mark device lost or try a soft return
            finalizeCpuTotals();
            return;
        }

        // Read GPU timestamp results for the *same frame slot* we just waited on.
        // This is the frame slot we are about to reuse, so its previous submission is guaranteed complete.
        // (Avoid reading a different slot which may still be in-flight and cause a stall.)
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE)
        {
            const auto queryT0 = Clock::now();

            const uint32_t completedStartQuery = m_currentFrame * 2;
            uint64_t timestamps[2] = {0, 0};
            VkResult queryResult = vkGetQueryPoolResults(
                m_device,
                m_timestampQueryPool,
                completedStartQuery,
                2,
                sizeof(timestamps),
                timestamps,
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT);

            if (queryResult == VK_SUCCESS && timestamps[1] > timestamps[0])
            {
                const uint64_t ticksDelta = timestamps[1] - timestamps[0];
                const float nanoseconds = static_cast<float>(ticksDelta) * m_timestampPeriod;
                m_gpuTimeMs = nanoseconds / 1000000.0f;
            }

            const auto queryT1 = Clock::now();
            m_cpuTimings.queryResultsMs = msSince(queryT0, queryT1);
        }

        // Acquire next image
        t0 = Clock::now();
        uint32_t imageIndex = 0;
        VkResult acquireRes = vkAcquireNextImageKHR(
            m_device,
            m_swapchain->GetSwapchain(),
            UINT64_MAX,
            frame.imageAcquiredSemaphore,
            VK_NULL_HANDLE,
            &imageIndex);
        t1 = Clock::now();
        m_cpuTimings.acquireMs = msSince(t0, t1);

        if (acquireRes == VK_ERROR_OUT_OF_DATE_KHR)
        {
            // Window resized or swapchain invalid -> recreate and skip this frame
            m_swapchain->Recreate(m_extent);
            // Framebuffers/renderpass depend on swapchain.
            recreateSwapchainDependent();
            finalizeCpuTotals();
            return; // IMPORTANT: we did NOT reset the fence, so next frame’s wait will pass.
        }
        if (acquireRes != VK_SUCCESS && acquireRes != VK_SUBOPTIMAL_KHR)
        {
            fprintf(stderr, "vkAcquireNextImageKHR failed: %d\n", acquireRes);
            finalizeCpuTotals();
            return; // Do not reset the fence on failure paths
        }

        // Record command buffer
        t0 = Clock::now();
        vkResetCommandBuffer(frame.commandBuffer, 0);
        t1 = Clock::now();
        m_cpuTimings.cmdResetMs = msSince(t0, t1);

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        t0 = Clock::now();
        vkBeginCommandBuffer(frame.commandBuffer, &beginInfo);
        t1 = Clock::now();
        m_cpuTimings.cmdBeginMs = msSince(t0, t1);

        t0 = Clock::now();

        // GPU timestamp: reset queries for this frame
        const uint32_t startQuery = m_currentFrame * 2;
        const uint32_t endQuery = startQuery + 1;
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkCmdResetQueryPool(frame.commandBuffer, m_timestampQueryPool, startQuery, 2);
            // Write start timestamp (at top of pipe for earliest possible time)
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, m_timestampQueryPool, startQuery);
        }
        t1 = Clock::now();
        m_cpuTimings.timestampResetMs = msSince(t0, t1);

        // Begin render pass
        VkClearValue clears[2]{};
        clears[0].color = {{0.02f, 0.02f, 0.04f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};

        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = m_mainRenderPass;
        rpBegin.framebuffer = m_framebuffers[imageIndex];
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = m_extent;
        rpBegin.clearValueCount = 2;
        rpBegin.pClearValues = clears;

        t0 = Clock::now();
        vkCmdBeginRenderPass(frame.commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
        t1 = Clock::now();
        m_cpuTimings.renderPassBeginMs = msSince(t0, t1);

        // Let modules record draw commands
        t0 = Clock::now();
        for (size_t i = 0; i < m_passes.size(); ++i)
        {
            auto &p = m_passes[i];
            if (!p)
                continue;

            const auto passT0 = Clock::now();
            p->record(frame, frame.commandBuffer);
            const auto passT1 = Clock::now();

            m_passCpuTimings[i].name = p->getDebugName();
            m_passCpuTimings[i].recordMs = msSince(passT0, passT1);
        }
        t1 = Clock::now();
        m_cpuTimings.passesRecordMs = msSince(t0, t1);

        // Render ImGui if callback is set
        t0 = Clock::now();
        if (m_imguiRenderCallback)
        {
            m_imguiRenderCallback(frame.commandBuffer);
        }
        t1 = Clock::now();
        m_cpuTimings.imguiRecordMs = msSince(t0, t1);

        t0 = Clock::now();
        vkCmdEndRenderPass(frame.commandBuffer);
        t1 = Clock::now();
        m_cpuTimings.renderPassEndMs = msSince(t0, t1);

        // GPU timestamp: write end timestamp (at bottom of pipe for latest possible time)
        if (m_timestampsSupported && m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkCmdWriteTimestamp(frame.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_timestampQueryPool, endQuery);
        }

        t0 = Clock::now();
        vkEndCommandBuffer(frame.commandBuffer);
        t1 = Clock::now();
        m_cpuTimings.cmdEndMs = msSince(t0, t1);

        // Full record bucket (for backwards-compatible display)
        m_cpuTimings.recordMs =
            m_cpuTimings.cmdResetMs +
            m_cpuTimings.cmdBeginMs +
            m_cpuTimings.timestampResetMs +
            m_cpuTimings.renderPassBeginMs +
            m_cpuTimings.passesRecordMs +
            m_cpuTimings.imguiRecordMs +
            m_cpuTimings.renderPassEndMs +
            m_cpuTimings.cmdEndMs;

        // Submit to graphics queue
        t0 = Clock::now();
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT};
        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = &frame.imageAcquiredSemaphore;
        submitInfo.pWaitDstStageMask = waitStages;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &frame.commandBuffer;
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = &frame.renderFinishedSemaphore;

        vkResetFences(m_device, 1, &frame.inFlightFence);
        vkQueueSubmit(m_graphicsQueue, 1, &submitInfo, frame.inFlightFence);
        t1 = Clock::now();
        m_cpuTimings.submitMs = msSince(t0, t1);

        // Present
        t0 = Clock::now();
        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = &frame.renderFinishedSemaphore;
        VkSwapchainKHR swapchains[] = {m_swapchain->GetSwapchain()};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapchains;
        presentInfo.pImageIndices = &imageIndex;

        vkQueuePresentKHR(m_presentQueue, &presentInfo);
        t1 = Clock::now();
        m_cpuTimings.presentMs = msSince(t0, t1);

        finalizeCpuTotals();
        // Advance frame index
        m_currentFrame = (m_currentFrame + 1) % m_maxFrames;
    }

    bool Renderer::waitForCurrentFrameFence()
    {
        if (!m_initialized)
            return false;
        if (m_frames.empty())
            return false;
        if (m_currentFrame >= m_frames.size())
            return false;

        FrameContext &frame = m_frames[m_currentFrame];
        VkResult r = vkWaitForFences(m_device, 1, &frame.inFlightFence, VK_TRUE, UINT64_MAX);
        return r == VK_SUCCESS;
    }

    void Renderer::destroySyncObjects()
    {
        for (auto &f : m_frames)
        {
            if (f.imageAcquiredSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, f.imageAcquiredSemaphore, nullptr);
                f.imageAcquiredSemaphore = VK_NULL_HANDLE;
            }
            if (f.renderFinishedSemaphore != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(m_device, f.renderFinishedSemaphore, nullptr);
                f.renderFinishedSemaphore = VK_NULL_HANDLE;
            }
            if (f.inFlightFence != VK_NULL_HANDLE)
            {
                vkDestroyFence(m_device, f.inFlightFence, nullptr);
                f.inFlightFence = VK_NULL_HANDLE;
            }
        }
    }

    void Renderer::destroyCommandPoolsAndBuffers()
    {
        for (auto &f : m_frames)
        {
            if (f.commandPool != VK_NULL_HANDLE)
            {
                vkDestroyCommandPool(m_device, f.commandPool, nullptr);
                f.commandPool = VK_NULL_HANDLE;
                f.commandBuffer = VK_NULL_HANDLE;
            }
        }
    }

    void Renderer::createTimestampQueryPool()
    {
        destroyTimestampQueryPool();

        // Check if the device supports timestamps
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(m_ctx->GetPhysicalDevice(), &props);

        if (props.limits.timestampComputeAndGraphics == VK_FALSE)
        {
            m_timestampsSupported = false;
            return;
        }

        m_timestampPeriod = props.limits.timestampPeriod; // Nanoseconds per tick
        m_timestampsSupported = true;

        // Create a query pool with 2 queries per frame (start and end)
        // We use 2 * maxFrames to have per-frame queries
        VkQueryPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        poolInfo.queryCount = m_maxFrames * 2; // 2 timestamps per frame

        if (vkCreateQueryPool(m_device, &poolInfo, nullptr, &m_timestampQueryPool) != VK_SUCCESS)
        {
            m_timestampsSupported = false;
            m_timestampQueryPool = VK_NULL_HANDLE;
        }
    }

    void Renderer::destroyTimestampQueryPool()
    {
        if (m_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(m_device, m_timestampQueryPool, nullptr);
            m_timestampQueryPool = VK_NULL_HANDLE;
        }
        m_timestampsSupported = false;
    }
}