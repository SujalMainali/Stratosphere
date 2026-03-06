#include "ECS/EcsTrace.h"

namespace Engine::ECS
{
    void EcsTrace::beginFrame()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_events.clear();
        m_eventIndexByName.clear();
    }

    EcsTrace::SystemEvent &EcsTrace::getOrCreateSystem(const char *name)
    {
        // Caller must hold m_mutex.
        const std::string key = (name ? std::string(name) : std::string("<null>"));
        auto it = m_eventIndexByName.find(key);
        if (it != m_eventIndexByName.end())
            return m_events[it->second];

        const size_t idx = m_events.size();
        m_events.push_back(SystemEvent{});
        m_events.back().name = key;
        m_eventIndexByName.emplace(key, idx);
        return m_events.back();
    }

    void EcsTrace::onSystemEnd(const char *name, float ms, uint32_t matchingArchetypes, uint32_t entitiesScanned)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SystemEvent &e = getOrCreateSystem(name);
        e.ms = ms;
        e.matchingArchetypes = matchingArchetypes;
        e.entitiesScanned = entitiesScanned;
        // dirtyRowsConsumed is accumulated via onDirtyConsumed.
    }

    void EcsTrace::onDirtyConsumed(const char *systemName, uint32_t archetypeId, uint32_t dirtyCount)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        SystemEvent &e = getOrCreateSystem(systemName);
        e.dirtyRowsConsumed += dirtyCount;
        if (dirtyCount > e.topDirtyRows)
        {
            e.topDirtyRows = dirtyCount;
            e.topDirtyArchetypeId = archetypeId;
        }
    }
}
