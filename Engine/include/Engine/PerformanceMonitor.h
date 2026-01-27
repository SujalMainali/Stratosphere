#pragma once

#include <chrono>
#include <deque>
#include <cstdint>
#include <string>

namespace Engine
{
    class Window;
    class Renderer;
    class VulkanContext;

    /**
     * @brief Performance monitoring system that tracks and displays real-time metrics.
     * 
     * Collects FPS, frame times, draw calls, and system information.
     * Renders an ImGui-based overlay when enabled.
     */
    class PerformanceMonitor
    {
    public:
        PerformanceMonitor();
        ~PerformanceMonitor();

        // Disable copy/move for singleton-like usage
        PerformanceMonitor(const PerformanceMonitor&) = delete;
        PerformanceMonitor& operator=(const PerformanceMonitor&) = delete;

        /**
         * @brief Initialize the performance monitor.
         * @param ctx Vulkan context for GPU queries
         * @param renderer Renderer for swapchain/extent info
         * @param window Window for resolution info
         */
        void init(VulkanContext* ctx, Renderer* renderer, Window* window);

        /**
         * @brief Cleanup resources.
         */
        void cleanup();

        /**
         * @brief Begin frame timing. Call at the start of each frame.
         */
        void beginFrame();

        /**
         * @brief End frame timing. Call at the end of each frame.
         */
        void endFrame();

        /**
         * @brief Record a draw call. Call each time vkCmdDraw* is invoked.
         * @param primitiveCount Number of primitives in this draw call
         */
        void recordDrawCall(uint32_t primitiveCount = 0);

        /**
         * @brief Reset draw call counter. Called at frame start.
         */
        void resetDrawCalls();

        /**
         * @brief Toggle the overlay visibility.
         */
        void toggle();

        /**
         * @brief Check if overlay is visible.
         */
        bool isVisible() const { return m_visible; }

        /**
         * @brief Set overlay visibility.
         */
        void setVisible(bool visible) { m_visible = visible; }

        /**
         * @brief Render the ImGui overlay. Call during UI rendering phase.
         */
        void renderOverlay();

        // Getters for current metrics
        float getAverageFPS() const { return m_avgFPS; }
        float get1PercentLowFPS() const { return m_1percentLowFPS; }
        float get01PercentLowFPS() const { return m_01percentLowFPS; }
        float getFrameTimeMs() const { return m_frameTimeMs; }
        float getCPUTimeMs() const { return m_cpuTimeMs; }
        uint32_t getDrawCallCount() const { return m_drawCallCount; }
        uint32_t getResolutionWidth() const;
        uint32_t getResolutionHeight() const;

    private:
        void updateMetrics();
        void calculatePercentileFPS();

    private:
        // References to engine systems
        VulkanContext* m_ctx = nullptr;
        Renderer* m_renderer = nullptr;
        Window* m_window = nullptr;

        // Visibility toggle
        bool m_visible = false;
        bool m_initialized = false;

        // Timing
        using Clock = std::chrono::high_resolution_clock;
        using TimePoint = std::chrono::time_point<Clock>;
        
        TimePoint m_frameStart;
        TimePoint m_lastFrameEnd;
        
        // Frame time history for percentile calculations (stores frame times in ms)
        static constexpr size_t HISTORY_SIZE = 300; // ~5 seconds at 60fps
        std::deque<float> m_frameTimeHistory;

        // Current metrics (raw values)
        float m_avgFPS = 0.0f;
        float m_1percentLowFPS = 0.0f;
        float m_01percentLowFPS = 0.0f;
        float m_frameTimeMs = 0.0f;
        float m_cpuTimeMs = 0.0f;
        float m_gpuTimeMs = 0.0f;  // Placeholder - requires GPU timestamp queries
        float m_gpuUsagePercent = 0.0f; // Placeholder - requires vendor-specific queries

        // Smoothed display values (EMA filtered for readability)
        float m_smoothedFrameTimeMs = 0.0f;
        float m_smoothedCpuTimeMs = 0.0f;
        float m_smoothedGpuTimeMs = 0.0f;

        // EMA smoothing factor: 0.1 = very smooth (slow response), 0.3 = moderate, 0.5 = responsive
        static constexpr float EMA_SMOOTHING_FACTOR = 0.15f;

        // Draw call tracking
        uint32_t m_drawCallCount = 0;
        uint32_t m_lastFrameDrawCalls = 0;
        uint32_t m_primitiveCount = 0;

        // Metrics update interval
        float m_updateTimer = 0.0f;
        static constexpr float UPDATE_INTERVAL = 0.1f; // Update every 100ms
    };

    /**
     * @brief Global draw call counter - increment from render modules.
     * Thread-safe atomic counter reset each frame.
     */
    class DrawCallCounter
    {
    public:
        static void increment(uint32_t count = 1);
        static void reset();
        static uint32_t get();
    };

} // namespace Engine
