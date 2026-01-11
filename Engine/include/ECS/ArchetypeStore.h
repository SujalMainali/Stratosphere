#pragma once
/*
  ArchetypeStore.h
  ----------------
  Purpose:
    - Provide a generic Struct-of-Arrays store for a single archetype (signature).
    - Conditionally hold arrays for components present in the signature (Position, Velocity, Health).
    - Support creation of rows with defaults, destruction via swap-remove, and per-row masks.

  Usage:
    - Construct with a signature.
    - resolveKnownComponents(registry) to enable arrays for known components.
    - createRow(entity) then applyDefaults(row, defaults, registry).
    - destroyRow(row) with dense packing.
*/

#include <vector>
#include <unordered_map>
#include <memory>
#include <variant>
#include "ECS/Components.h"
#include "ECS/Entity.h"

namespace Engine::ECS
{
    class ArchetypeStore
    {
    public:
        explicit ArchetypeStore(const ComponentMask &signature)
            : m_signature(signature) {}

        // Create a new row for the given entity; returns row index.
        uint32_t createRow(Entity e)
        {
            const uint32_t row = static_cast<uint32_t>(m_entities.size());
            m_entities.emplace_back(e);
            // Initialize row mask: start with the store signature (you can add tags per row later)
            m_rowMasks.emplace_back(m_signature);

            // Conditionally create per-component entries based on signature bits.
            if (hasPosition())
                m_positions.emplace_back(Position{});
            if (hasVelocity())
                m_velocities.emplace_back(Velocity{});
            if (hasHealth())
                m_healths.emplace_back(Health{100.f});
            if (hasMoveTarget())
                m_moveTargets.emplace_back(MoveTarget{});
            if (hasMoveSpeed())
                m_moveSpeeds.emplace_back(MoveSpeed{});
            if (hasRadius())
                m_radii.emplace_back(Radius{});
            if (hasSeparation())
                m_separations.emplace_back(Separation{});
            if (hasAvoidanceParams())
                m_avoidanceParams.emplace_back(AvoidanceParams{});
            if (hasRenderMesh())
                m_renderMeshes.emplace_back(RenderMesh{});

            return row;
        }

        // Swap-remove a row; maintains dense arrays and updates ownership.
        void destroyRow(uint32_t row)
        {
            const uint32_t last = static_cast<uint32_t>(m_entities.size() - 1);
            if (row > last)
                return;

            auto swapErase = [&](auto &vec)
            {
                if (row != last)
                    vec[row] = std::move(vec[last]);
                vec.pop_back();
            };

            swapErase(m_entities);
            swapErase(m_rowMasks);
            if (hasPosition())
                swapErase(m_positions);
            if (hasVelocity())
                swapErase(m_velocities);
            if (hasHealth())
                swapErase(m_healths);
            if (hasMoveTarget())
                swapErase(m_moveTargets);
            if (hasMoveSpeed())
                swapErase(m_moveSpeeds);
            if (hasRadius())
                swapErase(m_radii);
            if (hasSeparation())
                swapErase(m_separations);
            if (hasAvoidanceParams())
                swapErase(m_avoidanceParams);
            if (hasRenderMesh())
                swapErase(m_renderMeshes);
        }

        // Apply typed defaults for a newly created row.
        void applyDefaults(uint32_t row, const std::unordered_map<uint32_t, DefaultValue> &defaults,
                           const ComponentRegistry & /*registry*/)
        {
            for (const auto &kv : defaults)
            {
                const uint32_t cid = kv.first;
                if (!m_signature.has(cid))
                    continue;

                if (std::holds_alternative<Position>(kv.second) && hasPosition())
                {
                    m_positions[row] = std::get<Position>(kv.second);
                }
                else if (std::holds_alternative<Velocity>(kv.second) && hasVelocity())
                {
                    m_velocities[row] = std::get<Velocity>(kv.second);
                }
                else if (std::holds_alternative<Health>(kv.second) && hasHealth())
                {
                    m_healths[row] = std::get<Health>(kv.second);
                }
                else if (std::holds_alternative<MoveTarget>(kv.second) && hasMoveTarget())
                {
                    m_moveTargets[row] = std::get<MoveTarget>(kv.second);
                }
                else if (std::holds_alternative<MoveSpeed>(kv.second) && hasMoveSpeed())
                {
                    m_moveSpeeds[row] = std::get<MoveSpeed>(kv.second);
                }
                else if (std::holds_alternative<Radius>(kv.second) && hasRadius())
                {
                    m_radii[row] = std::get<Radius>(kv.second);
                }
                else if (std::holds_alternative<Separation>(kv.second) && hasSeparation())
                {
                    m_separations[row] = std::get<Separation>(kv.second);
                }
                else if (std::holds_alternative<AvoidanceParams>(kv.second) && hasAvoidanceParams())
                {
                    m_avoidanceParams[row] = std::get<AvoidanceParams>(kv.second);
                }
                else if (std::holds_alternative<RenderMesh>(kv.second) && hasRenderMesh())
                {
                    m_renderMeshes[row] = std::get<RenderMesh>(kv.second);
                }
            }
        }

        // Accessors
        const ComponentMask &signature() const { return m_signature; }
        uint32_t size() const { return static_cast<uint32_t>(m_entities.size()); }

        // Row-level masks (e.g., exclude tags applied per row).
        std::vector<ComponentMask> &rowMasks() { return m_rowMasks; }
        const std::vector<ComponentMask> &rowMasks() const { return m_rowMasks; }

        // Component arrays (conditionally enabled).
        std::vector<Position> &positions() { return m_positions; }
        const std::vector<Position> &positions() const { return m_positions; }

        std::vector<Velocity> &velocities() { return m_velocities; }
        const std::vector<Velocity> &velocities() const { return m_velocities; }

        std::vector<Health> &healths() { return m_healths; }
        const std::vector<Health> &healths() const { return m_healths; }

        std::vector<MoveTarget> &moveTargets() { return m_moveTargets; }
        const std::vector<MoveTarget> &moveTargets() const { return m_moveTargets; }

        std::vector<MoveSpeed> &moveSpeeds() { return m_moveSpeeds; }
        const std::vector<MoveSpeed> &moveSpeeds() const { return m_moveSpeeds; }

        std::vector<Radius> &radii() { return m_radii; }
        const std::vector<Radius> &radii() const { return m_radii; }

        std::vector<Separation> &separations() { return m_separations; }
        const std::vector<Separation> &separations() const { return m_separations; }

        std::vector<AvoidanceParams> &avoidanceParams() { return m_avoidanceParams; }
        const std::vector<AvoidanceParams> &avoidanceParams() const { return m_avoidanceParams; }

        std::vector<RenderMesh> &renderMeshes() { return m_renderMeshes; }
        const std::vector<RenderMesh> &renderMeshes() const { return m_renderMeshes; }

        // Helpers
        bool hasPosition() const { return m_hasPosition; }
        bool hasVelocity() const { return m_hasVelocity; }
        bool hasHealth() const { return m_hasHealth; }
        bool hasMoveTarget() const { return m_hasMoveTarget; }
        bool hasMoveSpeed() const { return m_hasMoveSpeed; }
        bool hasRadius() const { return m_hasRadius; }
        bool hasSeparation() const { return m_hasSeparation; }
        bool hasAvoidanceParams() const { return m_hasAvoidanceParams; }
        bool hasRenderMesh() const { return m_hasRenderMesh; }

        // Resolve which known components are present in signature; enables arrays accordingly.
        void resolveKnownComponents(ComponentRegistry &registry)
        {
            const uint32_t posId = registry.ensureId("Position");
            const uint32_t velId = registry.ensureId("Velocity");
            const uint32_t heaId = registry.ensureId("Health");
            const uint32_t tgtId = registry.ensureId("MoveTarget");
            const uint32_t spdId = registry.ensureId("MoveSpeed");
            const uint32_t radId = registry.ensureId("Radius");
            const uint32_t sepId = registry.ensureId("Separation");
            const uint32_t apId = registry.ensureId("AvoidanceParams");
            const uint32_t rmId = registry.ensureId("RenderMesh");
            m_hasPosition = m_signature.has(posId);
            m_hasVelocity = m_signature.has(velId);
            m_hasHealth = m_signature.has(heaId);
            m_hasMoveTarget = m_signature.has(tgtId);
            m_hasMoveSpeed = m_signature.has(spdId);
            m_hasRadius = m_signature.has(radId);
            m_hasSeparation = m_signature.has(sepId);
            m_hasAvoidanceParams = m_signature.has(apId);
            m_hasRenderMesh = m_signature.has(rmId);
        }

    private:
        ComponentMask m_signature;
        std::vector<Entity> m_entities;
        std::vector<ComponentMask> m_rowMasks;

        // Component arrays (only used if signature includes them).
        std::vector<Position> m_positions;
        std::vector<Velocity> m_velocities;
        std::vector<Health> m_healths;
        std::vector<MoveTarget> m_moveTargets;
        std::vector<MoveSpeed> m_moveSpeeds;
        std::vector<Radius> m_radii;
        std::vector<Separation> m_separations;
        std::vector<AvoidanceParams> m_avoidanceParams;
        std::vector<RenderMesh> m_renderMeshes;

        // Flags indicating which arrays are active.
        bool m_hasPosition = false;
        bool m_hasVelocity = false;
        bool m_hasHealth = false;
        bool m_hasMoveTarget = false;
        bool m_hasMoveSpeed = false;
        bool m_hasRadius = false;
        bool m_hasSeparation = false;
        bool m_hasAvoidanceParams = false;
        bool m_hasRenderMesh = false;
    };

    class ArchetypeStoreManager
    {
    public:
        ArchetypeStore *getOrCreate(uint32_t archetypeId, const ComponentMask &signature, ComponentRegistry &registry)
        {
            if (archetypeId >= m_stores.size())
                m_stores.resize(archetypeId + 1);

            if (!m_stores[archetypeId])
            {
                m_stores[archetypeId] = std::make_unique<ArchetypeStore>(signature);
                m_stores[archetypeId]->resolveKnownComponents(registry);
            }
            return m_stores[archetypeId].get();
        }

        ArchetypeStore *get(uint32_t archetypeId)
        {
            return (archetypeId < m_stores.size()) ? m_stores[archetypeId].get() : nullptr;
        }

        const std::vector<std::unique_ptr<ArchetypeStore>> &stores() const { return m_stores; }

    private:
        std::vector<std::unique_ptr<ArchetypeStore>> m_stores;
    };

} // namespace Engine::ECS