#pragma once

#include "ECS/SystemFormat.h"
#include "ECS/VisibleRender.h"

#include "assets/AssetManager.h"

#include "Engine/Camera.h"
#include "Engine/Renderer.h"
#include "Engine/SModelRenderPassModule.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
#include <iostream>
#endif

class RenderSystem : public Engine::ECS::SystemBase
{
public:
    explicit RenderSystem(Engine::AssetManager *assets = nullptr)
        : m_assets(assets)
    {
        // RenderModel provides the model handle.
        // PosePalette provides per-entity node/joint palettes.
        // RenderTransform provides the cached world matrix.
        setRequiredNames({"RenderModel", "PosePalette", "RenderTransform"});
        setExcludedNames({"Disabled", "Dead"});
    }

    const char *name() const override { return "RenderModelSystem"; }
    void setAssetManager(Engine::AssetManager *assets) { m_assets = assets; }

    void buildMasks(Engine::ECS::ComponentRegistry &registry) override
    {
        Engine::ECS::SystemBase::buildMasks(registry);
    }

    void setRenderer(Engine::Renderer *renderer) { m_renderer = renderer; }
    void setCamera(Engine::Camera *camera) { m_camera = camera; }
    void setVisibleBuckets(const Engine::ECS::VisibleRenderBuckets *buckets) { m_visibleBuckets = buckets; }

    void update(Engine::ECS::ECSContext &ecs, float dt) override
    {
        (void)dt;
        if (!m_assets || !m_renderer || !m_camera || !m_visibleBuckets)
            return;

        auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
        {
            return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
        };

        m_frameCounter += 1u;

        Stats frameStats{};
        frameStats.totalCandidates = m_visibleBuckets->totalRenderables;
        frameStats.visibleProcessed = m_visibleBuckets->visibleRenderables;

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        const auto t0 = std::chrono::high_resolution_clock::now();
#endif

        // Drive passes directly from explicit visible buckets.
        for (uint64_t key : m_visibleBuckets->activeModelKeys)
        {
            auto itBucket = m_visibleBuckets->byModel.find(key);
            if (itBucket == m_visibleBuckets->byModel.end())
                continue;

            const Engine::ECS::VisibleModelBucket &bucket = itBucket->second;
            if (bucket.lastUsedFrame != m_visibleBuckets->frame)
                continue;
            if (bucket.refs.empty())
                continue;

            const Engine::ModelHandle handle = bucket.handle;
            Engine::ModelAsset *asset = m_assets->getModel(handle);
            if (!asset)
                continue;

            frameStats.visibleModelBatches += 1u;

            auto itPass = m_passes.find(key);
            if (itPass == m_passes.end())
            {
                PassEntry entry;
                entry.pass = std::make_shared<Engine::SModelRenderPassModule>();
                entry.pass->setAssets(m_assets);
                entry.pass->setModel(handle);
                entry.pass->setCamera(m_camera);
                entry.pass->setEnabled(true);
                m_renderer->registerPass(entry.pass);
                itPass = m_passes.emplace(key, std::move(entry)).first;
            }

            PassEntry &entry = itPass->second;
            entry.lastUsedFrame = m_frameCounter;
            entry.pass->setCamera(m_camera);
            entry.pass->setEnabled(true);

            const uint32_t nodeCount = std::max<uint32_t>(static_cast<uint32_t>(asset->nodes.size()), 1u);
            const uint32_t jointCount = asset->totalJointCount;
            entry.pass->setSlotLayout(nodeCount, jointCount);

            ModelSlotAllocator &alloc = entry.allocator;
            alloc.activeSlots.clear();
            alloc.activeSlots.reserve(bucket.refs.size());

            // Allocate/refresh slots for visible entities and push incremental updates for dirty slots.
            for (const Engine::ECS::VisibleRenderRef &ref : bucket.refs)
            {
                const uint64_t eKey = entityKey(ref.entity);

                auto itEnt = alloc.entityToSlot.find(eKey);
                bool isNew = false;
                if (itEnt == alloc.entityToSlot.end())
                {
                    uint32_t slot = 0;
                    if (!alloc.freeList.empty())
                    {
                        slot = alloc.freeList.back();
                        alloc.freeList.pop_back();
                        frameStats.reusedSlots += 1u;
                    }
                    else
                    {
                        slot = static_cast<uint32_t>(alloc.slotToEntity.size());
                        alloc.slotToEntity.push_back(Engine::ECS::Entity{});
                        alloc.slotSeenFrame.push_back(0u);
                        frameStats.newSlots += 1u;
                    }

                    alloc.slotToEntity[slot] = ref.entity;
                    alloc.slotSeenFrame[slot] = m_frameCounter;

                    EntitySlotState st;
                    st.slot = slot;
                    st.lastTransformVersion = kInvalidVersion;
                    st.lastPoseVersion = kInvalidVersion;
                    itEnt = alloc.entityToSlot.emplace(eKey, st).first;
                    isNew = true;
                }

                const uint32_t slot = itEnt->second.slot;
                if (slot >= alloc.slotSeenFrame.size())
                    continue;

                alloc.slotSeenFrame[slot] = m_frameCounter;
                alloc.activeSlots.push_back(slot);

                const bool newlyVisible = isNew || ref.justBecameVisible;
                const bool transformDirty = newlyVisible || (itEnt->second.lastTransformVersion != ref.transformVersion);
                const bool poseDirty = newlyVisible || (itEnt->second.lastPoseVersion != ref.poseVersion);

                if (transformDirty || poseDirty)
                {
                    glm::mat4 world(1.0f);
                    const Engine::ECS::PosePalette *posePtr = nullptr;

                    auto *storePtr = ecs.stores.get(ref.archetypeId);
                    if (storePtr && ref.row < storePtr->size() && storePtr->hasRenderTransform() && storePtr->hasPosePalette())
                    {
                        world = storePtr->renderTransforms()[ref.row].world;
                        posePtr = &storePtr->posePalettes()[ref.row];
                    }

                    // Ensure slot arrays exist on the render module.
                    entry.pass->ensureSlotCapacity(static_cast<uint32_t>(alloc.slotToEntity.size()));

                    if (transformDirty)
                    {
                        entry.pass->setSlotWorld(slot, world);
                        itEnt->second.lastTransformVersion = ref.transformVersion;
                        frameStats.transformSlotUpdates += 1u;
                    }

                    if (poseDirty)
                    {
                        if (posePtr)
                        {
                            const glm::mat4 *nodeSrc = posePtr->nodePalette.empty() ? nullptr : posePtr->nodePalette.data();
                            const glm::mat4 *jointSrc = posePtr->jointPalette.empty() ? nullptr : posePtr->jointPalette.data();
                            entry.pass->setSlotPose(slot, nodeSrc, posePtr->nodeCount, jointSrc, posePtr->jointCount);
                        }
                        else
                        {
                            entry.pass->setSlotPose(slot, nullptr, 0, nullptr, 0);
                        }
                        itEnt->second.lastPoseVersion = ref.poseVersion;
                        frameStats.poseSlotUpdates += 1u;
                    }
                }
            }

            // Free slots for entities that were visible last frame but not this frame.
            for (uint32_t slot : alloc.lastActiveSlots)
            {
                if (slot >= alloc.slotSeenFrame.size())
                    continue;
                if (alloc.slotSeenFrame[slot] == m_frameCounter)
                    continue;

                const Engine::ECS::Entity e = alloc.slotToEntity[slot];
                if (e.valid())
                {
                    const uint64_t eKey = entityKey(e);
                    alloc.entityToSlot.erase(eKey);
                }

                alloc.slotToEntity[slot] = Engine::ECS::Entity{};
                alloc.freeList.push_back(slot);
                frameStats.freedSlots += 1u;
            }

            alloc.lastActiveSlots = alloc.activeSlots;

            // Submit active slot list every frame (small SSBO).
            entry.pass->ensureSlotCapacity(static_cast<uint32_t>(alloc.slotToEntity.size()));
            entry.pass->setActiveSlots(alloc.activeSlots.data(), static_cast<uint32_t>(alloc.activeSlots.size()));

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
            if ((m_frameCounter % 240u) == 0u)
            {
                std::unordered_set<uint32_t> uniq;
                uniq.reserve(alloc.activeSlots.size());
                for (uint32_t s : alloc.activeSlots)
                    uniq.insert(s);
                if (uniq.size() != alloc.activeSlots.size())
                {
                    std::cout << "[RenderSystem] WARNING duplicate slots in active list for modelKey=" << key
                              << " active=" << alloc.activeSlots.size() << " unique=" << uniq.size() << "\n";
                }
            }
#endif
        }

        // Disable passes that are not used this frame.
        for (auto &kv : m_passes)
        {
            if (kv.second.lastUsedFrame != m_frameCounter)
            {
                kv.second.pass->setEnabled(false);
                kv.second.pass->setActiveSlots(nullptr, 0);

                // Inactive model => all instances invisible => free all slots.
                ModelSlotAllocator &alloc = kv.second.allocator;
                alloc.entityToSlot.clear();
                alloc.lastActiveSlots.clear();
                alloc.activeSlots.clear();

                alloc.freeList.clear();
                const uint32_t slots = static_cast<uint32_t>(alloc.slotToEntity.size());
                alloc.freeList.reserve(slots);
                for (uint32_t s = 0; s < slots; ++s)
                {
                    alloc.slotToEntity[s] = Engine::ECS::Entity{};
                    alloc.slotSeenFrame[s] = 0u;
                    alloc.freeList.push_back(s);
                }
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            const auto t1 = std::chrono::high_resolution_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
            std::cout << "[RenderSystem] frame=" << m_frameCounter
                      << " candidates=" << frameStats.totalCandidates
                      << " visible=" << frameStats.visibleProcessed
                      << " visibleBatches=" << frameStats.visibleModelBatches
                      << " slots(+new/reuse/free)=" << frameStats.newSlots << "/" << frameStats.reusedSlots << "/" << frameStats.freedSlots
                      << " slotUpdates(xform/pose)=" << frameStats.transformSlotUpdates << "/" << frameStats.poseSlotUpdates
                      << " updateMs=" << ms
                      << "\n";
        }
#endif
    }

private:
    struct Stats
    {
        uint32_t totalCandidates = 0;
        uint32_t visibleProcessed = 0;

        uint32_t visibleModelBatches = 0;

        uint32_t newSlots = 0;
        uint32_t reusedSlots = 0;
        uint32_t freedSlots = 0;

        uint32_t transformSlotUpdates = 0;
        uint32_t poseSlotUpdates = 0;
    };

    static constexpr uint32_t kInvalidVersion = std::numeric_limits<uint32_t>::max();

    struct EntitySlotState
    {
        uint32_t slot = 0;
        uint32_t lastTransformVersion = kInvalidVersion;
        uint32_t lastPoseVersion = kInvalidVersion;
    };

    struct ModelSlotAllocator
    {
        std::unordered_map<uint64_t, EntitySlotState> entityToSlot;
        std::vector<Engine::ECS::Entity> slotToEntity;
        std::vector<uint32_t> slotSeenFrame;
        std::vector<uint32_t> freeList;

        std::vector<uint32_t> lastActiveSlots;
        std::vector<uint32_t> activeSlots;
    };

    struct PassEntry
    {
        std::shared_ptr<Engine::SModelRenderPassModule> pass;
        uint32_t lastUsedFrame = 0;
        ModelSlotAllocator allocator;
    };

    Engine::AssetManager *m_assets = nullptr; // not owned
    Engine::Renderer *m_renderer = nullptr;   // not owned
    Engine::Camera *m_camera = nullptr;       // not owned
    const Engine::ECS::VisibleRenderBuckets *m_visibleBuckets = nullptr; // not owned

    std::unordered_map<uint64_t, PassEntry> m_passes;
    uint32_t m_frameCounter = 0;
};
