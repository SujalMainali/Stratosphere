#include "Engine/PerformanceMonitor.h"
#include "Engine/VulkanContext.h"
#include "Engine/Renderer.h"
#include "Engine/Window.h"

#include <imgui.h>
#include <algorithm>
#include <numeric>
#include <atomic>
#include <cmath>

namespace Engine
{
    // Global atomic draw call counter
    static std::atomic<uint32_t> g_drawCallCount{0};

    void DrawCallCounter::increment(uint32_t count)
    {
        g_drawCallCount.fetch_add(count, std::memory_order_relaxed);
    }

    void DrawCallCounter::reset()
    {
        g_drawCallCount.store(0, std::memory_order_relaxed);
    }

    uint32_t DrawCallCounter::get()
    {
        return g_drawCallCount.load(std::memory_order_relaxed);
    }

    PerformanceMonitor::PerformanceMonitor()
        : m_frameStart(Clock::now())
        , m_lastFrameEnd(Clock::now())
    {
    }

    PerformanceMonitor::~PerformanceMonitor()
    {
        cleanup();
    }

    void PerformanceMonitor::init(VulkanContext* ctx, Renderer* renderer, Window* window)
    {
        m_ctx = ctx;
        m_renderer = renderer;
        m_window = window;
        m_initialized = true;
        m_frameTimeHistory.clear();
    }

    void PerformanceMonitor::cleanup()
    {
        m_initialized = false;
        m_frameTimeHistory.clear();
    }

    void PerformanceMonitor::beginFrame()
    {
        m_frameStart = Clock::now();
        DrawCallCounter::reset();
    }

    void PerformanceMonitor::endFrame()
    {
        auto now = Clock::now();
        
        // Calculate frame time
        float frameTimeMs = std::chrono::duration<float, std::milli>(now - m_lastFrameEnd).count();
        m_lastFrameEnd = now;
        
        // CPU time is the time spent between beginFrame and endFrame
        m_cpuTimeMs = std::chrono::duration<float, std::milli>(now - m_frameStart).count();
        
        // Store frame time in history
        m_frameTimeHistory.push_back(frameTimeMs);
        if (m_frameTimeHistory.size() > HISTORY_SIZE)
        {
            m_frameTimeHistory.pop_front();
        }

        // Get draw call count from global counter
        m_lastFrameDrawCalls = DrawCallCounter::get();

        // Get GPU time from renderer if available
        if (m_renderer)
        {
            m_gpuTimeMs = m_renderer->getGpuTimeMs();
        }

        // Apply EMA (Exponential Moving Average) smoothing for display values
        // Formula: smoothed = alpha * current + (1 - alpha) * smoothed_previous
        // This provides stable, readable values while remaining responsive to changes
        m_smoothedFrameTimeMs = EMA_SMOOTHING_FACTOR * frameTimeMs + 
                                (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedFrameTimeMs;
        m_smoothedCpuTimeMs = EMA_SMOOTHING_FACTOR * m_cpuTimeMs + 
                              (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedCpuTimeMs;
        m_smoothedGpuTimeMs = EMA_SMOOTHING_FACTOR * m_gpuTimeMs + 
                              (1.0f - EMA_SMOOTHING_FACTOR) * m_smoothedGpuTimeMs;

        // Update metrics periodically
        m_updateTimer += frameTimeMs / 1000.0f;
        if (m_updateTimer >= UPDATE_INTERVAL)
        {
            updateMetrics();
            m_updateTimer = 0.0f;
        }

        m_frameTimeMs = frameTimeMs;
    }

    void PerformanceMonitor::recordDrawCall(uint32_t primitiveCount)
    {
        DrawCallCounter::increment(1);
        m_primitiveCount += primitiveCount;
    }

    void PerformanceMonitor::resetDrawCalls()
    {
        DrawCallCounter::reset();
        m_primitiveCount = 0;
    }

    void PerformanceMonitor::toggle()
    {
        m_visible = !m_visible;
    }

    void PerformanceMonitor::updateMetrics()
    {
        if (m_frameTimeHistory.empty())
            return;

        // Calculate average FPS
        float totalTime = 0.0f;
        for (float t : m_frameTimeHistory)
        {
            totalTime += t;
        }
        float avgFrameTime = totalTime / static_cast<float>(m_frameTimeHistory.size());
        m_avgFPS = (avgFrameTime > 0.0f) ? (1000.0f / avgFrameTime) : 0.0f;

        // Calculate percentile FPS
        calculatePercentileFPS();
    }

    void PerformanceMonitor::calculatePercentileFPS()
    {
        if (m_frameTimeHistory.size() < 10)
        {
            m_1percentLowFPS = m_avgFPS;
            m_01percentLowFPS = m_avgFPS;
            return;
        }

        // Copy and sort frame times (descending - longest times first = worst frames)
        std::vector<float> sortedTimes(m_frameTimeHistory.begin(), m_frameTimeHistory.end());
        std::sort(sortedTimes.begin(), sortedTimes.end(), std::greater<float>());

        // 1% low = average of worst 1% of frames
        size_t onePercentCount = std::max(static_cast<size_t>(1), sortedTimes.size() / 100);
        float sum1Percent = 0.0f;
        for (size_t i = 0; i < onePercentCount; ++i)
        {
            sum1Percent += sortedTimes[i];
        }
        float avg1PercentTime = sum1Percent / static_cast<float>(onePercentCount);
        m_1percentLowFPS = (avg1PercentTime > 0.0f) ? (1000.0f / avg1PercentTime) : 0.0f;

        // 0.1% low = the single worst frame (or average of worst 0.1%)
        size_t point1PercentCount = std::max(static_cast<size_t>(1), sortedTimes.size() / 1000);
        float sum01Percent = 0.0f;
        for (size_t i = 0; i < point1PercentCount; ++i)
        {
            sum01Percent += sortedTimes[i];
        }
        float avg01PercentTime = sum01Percent / static_cast<float>(point1PercentCount);
        m_01percentLowFPS = (avg01PercentTime > 0.0f) ? (1000.0f / avg01PercentTime) : 0.0f;
    }

    uint32_t PerformanceMonitor::getResolutionWidth() const
    {
        if (m_window)
            return m_window->GetWidth();
        return 0;
    }

    uint32_t PerformanceMonitor::getResolutionHeight() const
    {
        if (m_window)
            return m_window->GetHeight();
        return 0;
    }

    void PerformanceMonitor::renderOverlay()
    {
        if (!m_visible || !m_initialized)
            return;

        // Set up overlay window flags
        ImGuiWindowFlags windowFlags = 
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav |
            ImGuiWindowFlags_NoMove;

        // Position in top-right corner with padding
        const float padding = 10.0f;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 workPos = viewport->WorkPos;
        ImVec2 workSize = viewport->WorkSize;
        ImVec2 windowPos(workPos.x + workSize.x - padding, workPos.y + padding);
        ImVec2 windowPivot(1.0f, 0.0f);
        ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always, windowPivot);
        ImGui::SetNextWindowBgAlpha(0.75f);

        if (ImGui::Begin("Performance Monitor", nullptr, windowFlags))
        {
            // Title with styling
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 0.4f, 1.0f));
            ImGui::Text("Performance Monitor");
            ImGui::PopStyleColor();
            ImGui::Separator();

            // FPS Section
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("FPS");
            ImGui::PopStyleColor();
            
            // Color-code FPS based on performance
            ImVec4 fpsColor;
            if (m_avgFPS >= 60.0f)
                fpsColor = ImVec4(0.4f, 1.0f, 0.4f, 1.0f); // Green
            else if (m_avgFPS >= 30.0f)
                fpsColor = ImVec4(1.0f, 1.0f, 0.4f, 1.0f); // Yellow
            else
                fpsColor = ImVec4(1.0f, 0.4f, 0.4f, 1.0f); // Red

            ImGui::PushStyleColor(ImGuiCol_Text, fpsColor);
            ImGui::Text("  Average: %.1f", m_avgFPS);
            ImGui::PopStyleColor();
            ImGui::Text("  1%% Low:  %.1f", m_1percentLowFPS);
            ImGui::Text("  0.1%% Low: %.1f", m_01percentLowFPS);

            ImGui::Spacing();

            // Frame Time Section
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Frame Time");
            ImGui::PopStyleColor();
            // Display EMA-smoothed values for stable, readable metrics
            ImGui::Text("  Frame: %.2f ms", m_smoothedFrameTimeMs);
            ImGui::Text("  CPU:   %.2f ms", m_smoothedCpuTimeMs);
            
            // GPU time from Vulkan timestamp queries (also EMA-smoothed)
            if (m_gpuTimeMs > 0.0f)
            {
                ImGui::Text("  GPU:   %.2f ms", m_smoothedGpuTimeMs);
            }
            else
            {
                ImGui::TextDisabled("  GPU:   N/A");
            }

            ImGui::Spacing();

            // Resolution & Refresh Rate
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Display");
            ImGui::PopStyleColor();
            ImGui::Text("  Resolution: %ux%u", getResolutionWidth(), getResolutionHeight());
            
            // Estimate refresh rate from inverse of target frame time (simplified)
            float estimatedRefreshRate = (m_avgFPS > 0.0f) ? std::min(m_avgFPS, 144.0f) : 60.0f;
            ImGui::TextDisabled("  Refresh: ~%.0f Hz", estimatedRefreshRate);

            ImGui::Spacing();

            // Draw Calls Section
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f));
            ImGui::Text("Rendering");
            ImGui::PopStyleColor();
            ImGui::Text("  Draw Calls: %u", m_lastFrameDrawCalls);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextDisabled("Press F1 to toggle");
        }
        ImGui::End();
    }

} // namespace Engine
