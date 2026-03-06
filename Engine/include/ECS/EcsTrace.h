#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Engine::ECS
{
    // Debug-only: collects per-frame schedule trace and summary stats.
    // Designed to be cheap enough for debug builds but compiled out in production.
    class EcsTrace
    {
    public:
        struct SystemEvent
        {
            std::string name;
            float ms = 0.0f;
            uint32_t matchingArchetypes = 0;
            uint32_t entitiesScanned = 0;
            uint32_t dirtyRowsConsumed = 0;

            // For quick diagnosis: the single worst archetype for this system (by dirty rows consumed).
            uint32_t topDirtyArchetypeId = 0;
            uint32_t topDirtyRows = 0;
        };

        void beginFrame();

        // Scheduler hooks
        void onSystemEnd(const char *name, float ms, uint32_t matchingArchetypes, uint32_t entitiesScanned);

        // QueryManager hook
        void onDirtyConsumed(const char *systemName, uint32_t archetypeId, uint32_t dirtyCount);

        const std::vector<SystemEvent> &events() const { return m_events; }

    private:
        SystemEvent &getOrCreateSystem(const char *name);

    private:
        std::vector<SystemEvent> m_events;
        std::unordered_map<std::string, size_t> m_eventIndexByName;
    };
}
