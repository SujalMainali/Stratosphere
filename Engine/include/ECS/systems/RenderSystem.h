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

        Stats frameStats{};

        auto entityKey = [](const Engine::ECS::Entity &e) -> uint64_t
        {
            return (static_cast<uint64_t>(e.generation) << 32) | static_cast<uint64_t>(e.index);
        };

        auto fnv1aMixU32 = [](uint64_t h, uint32_t v) -> uint64_t
        {
            // FNV-1a 64-bit (mix 4 bytes in a stable way)
            h ^= static_cast<uint64_t>(v) & 0xFFu;
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 8) & 0xFFu);
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 16) & 0xFFu);
            h *= 1099511628211ull;
            h ^= static_cast<uint64_t>((v >> 24) & 0xFFu);
            h *= 1099511628211ull;
            return h;
        };

        m_frameCounter += 1u;

        frameStats.totalCandidates = m_visibleBuckets->totalRenderables;
        frameStats.visibleProcessed = m_visibleBuckets->visibleRenderables;

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        const auto tBuild0 = std::chrono::high_resolution_clock::now();
#endif

        // Build render batches from explicit visible buckets.
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

            frameStats.visibleModelBatches += 1u;

            const Engine::ModelHandle handle = bucket.handle;
            Engine::ModelAsset *asset = m_assets->getModel(handle);
            if (!asset)
                continue;

            auto &batch = m_batchesByModel[key];
            batch.lastUsedFrame = m_frameCounter;
            batch.handle = handle;
            batch.instanceCount = static_cast<uint32_t>(bucket.refs.size());

            // Default counts from the asset; PosePalette data is validated per instance.
            batch.nodeCount = static_cast<uint32_t>(asset->nodes.size());
            batch.jointCount = asset->totalJointCount;

            // Membership hash encodes entity ordering + count.
            batch.membershipHash = 1469598103934665603ull;
            batch.membershipHash = fnv1aMixU32(batch.membershipHash, static_cast<uint32_t>(handle.id));
            batch.membershipHash = fnv1aMixU32(batch.membershipHash, static_cast<uint32_t>(handle.generation));
            batch.membershipHash = fnv1aMixU32(batch.membershipHash, static_cast<uint32_t>(bucket.refs.size()));

            // Separate version hashes (only meaningful when membership stays constant).
            batch.transformHash = 1469598103934665603ull;
            batch.poseHash = 1469598103934665603ull;

            batch.anyTransformDirty = false;
            batch.anyPoseDirty = false;
            batch.dirtyTransformEntities = 0;
            batch.dirtyPoseEntities = 0;
            batch.newlyVisibleEntities = 0;

            // Decide dirty-ness using per-entity upload state.
            for (const Engine::ECS::VisibleRenderRef &ref : bucket.refs)
            {
                batch.membershipHash = fnv1aMixU32(batch.membershipHash, ref.entity.index);
                batch.membershipHash = fnv1aMixU32(batch.membershipHash, ref.entity.generation);

                batch.transformHash = fnv1aMixU32(batch.transformHash, ref.entity.index);
                batch.transformHash = fnv1aMixU32(batch.transformHash, ref.entity.generation);
                batch.transformHash = fnv1aMixU32(batch.transformHash, ref.transformVersion);

                batch.poseHash = fnv1aMixU32(batch.poseHash, ref.entity.index);
                batch.poseHash = fnv1aMixU32(batch.poseHash, ref.entity.generation);
                batch.poseHash = fnv1aMixU32(batch.poseHash, ref.poseVersion);

                const uint64_t eKey = entityKey(ref.entity);
                auto itState = m_uploadStates.find(eKey);
                if (itState == m_uploadStates.end())
                {
                    PerEntityUploadState st;
                    st.lastUploadedTransformVersion = kInvalidVersion;
                    st.lastUploadedPoseVersion = kInvalidVersion;
                    st.lastUploadedVisibleFrame = 0;
                    st.lastSeenFrame = m_frameCounter;
                    itState = m_uploadStates.emplace(eKey, st).first;
                }
                else
                {
                    itState->second.lastSeenFrame = m_frameCounter;
                }

                const bool newlyVisible = ref.justBecameVisible;
                if (newlyVisible)
                    batch.newlyVisibleEntities += 1u;

                const bool transformDirty = newlyVisible || (itState->second.lastUploadedTransformVersion != ref.transformVersion);
                const bool poseDirty = newlyVisible || (itState->second.lastUploadedPoseVersion != ref.poseVersion);

                if (transformDirty)
                {
                    batch.anyTransformDirty = true;
                    batch.dirtyTransformEntities += 1u;
                }
                if (poseDirty)
                {
                    batch.anyPoseDirty = true;
                    batch.dirtyPoseEntities += 1u;
                }
            }

            // Determine whether we must rebuild/upload data for this batch.
            auto itPass = m_passes.find(key);
            const bool isNewPass = (itPass == m_passes.end());
            const uint64_t prevMembershipHash = isNewPass ? 0ull : itPass->second.lastSubmittedMembershipHash;
            const bool membershipChanged = isNewPass || (prevMembershipHash != batch.membershipHash);
            batch.membershipChanged = membershipChanged;

            // Without slot allocation, any membership change forces full re-upload.
            const uint64_t prevTransformHash = isNewPass ? 0ull : itPass->second.lastSubmittedTransformHash;
            const uint64_t prevPoseHash = isNewPass ? 0ull : itPass->second.lastSubmittedPoseHash;
            batch.uploadTransforms = membershipChanged || (prevTransformHash != batch.transformHash);
            batch.uploadPoses = membershipChanged || (prevPoseHash != batch.poseHash);

            if (batch.uploadTransforms)
            {
                batch.instanceWorlds.clear();
                batch.instanceWorlds.reserve(bucket.refs.size());
            }
            if (batch.uploadPoses)
            {
                batch.nodePalette.clear();
                batch.jointPalette.clear();
                batch.nodePalette.reserve(bucket.refs.size() * static_cast<size_t>(batch.nodeCount));
                if (batch.jointCount > 0)
                    batch.jointPalette.reserve(bucket.refs.size() * static_cast<size_t>(batch.jointCount));
            }

            if (batch.nodeCount == 0)
            {
                // Nothing sensible to render for this model.
                batch.instanceCount = 0;
                continue;
            }

            // Only build the CPU buffers that are going to be uploaded.
            if (!batch.uploadTransforms && !batch.uploadPoses)
                continue;

            for (const Engine::ECS::VisibleRenderRef &ref : bucket.refs)
            {
                glm::mat4 world(1.0f);
                const Engine::ECS::PosePalette *posePtr = nullptr;

                auto *storePtr = ecs.stores.get(ref.archetypeId);
                if (storePtr && ref.row < storePtr->size() && storePtr->hasRenderTransform() && storePtr->hasPosePalette())
                {
                    world = storePtr->renderTransforms()[ref.row].world;
                    posePtr = &storePtr->posePalettes()[ref.row];
                }

                if (batch.uploadTransforms)
                {
                    batch.instanceWorlds.emplace_back(world);
                }

                if (batch.uploadPoses)
                {
                    if (posePtr && posePtr->nodeCount == batch.nodeCount && posePtr->nodePalette.size() == static_cast<size_t>(batch.nodeCount))
                        batch.nodePalette.insert(batch.nodePalette.end(), posePtr->nodePalette.begin(), posePtr->nodePalette.end());
                    else
                        batch.nodePalette.insert(batch.nodePalette.end(), batch.nodeCount, glm::mat4(1.0f));

                    if (batch.jointCount > 0)
                    {
                        if (posePtr && posePtr->jointCount == batch.jointCount && posePtr->jointPalette.size() == static_cast<size_t>(batch.jointCount))
                            batch.jointPalette.insert(batch.jointPalette.end(), posePtr->jointPalette.begin(), posePtr->jointPalette.end());
                        else
                            batch.jointPalette.insert(batch.jointPalette.end(), batch.jointCount, glm::mat4(1.0f));
                    }
                }
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        const auto tBuild1 = std::chrono::high_resolution_clock::now();
#endif

        // Create/update passes for models that have instances this frame.
        for (auto &kv : m_batchesByModel)
        {
            const uint64_t key = kv.first;
            auto &batch = kv.second;
            if (batch.lastUsedFrame != m_frameCounter)
                continue;
            if (batch.instanceCount == 0)
                continue;

            const Engine::ModelHandle handle = batch.handle;

            auto it = m_passes.find(key);
            if (it == m_passes.end())
            {
                PassEntry entry;
                entry.pass = std::make_shared<Engine::SModelRenderPassModule>();
                entry.pass->setAssets(m_assets);
                entry.pass->setModel(handle);
                entry.pass->setCamera(m_camera);
                entry.pass->setEnabled(true);
                m_renderer->registerPass(entry.pass);
                it = m_passes.emplace(key, std::move(entry)).first;
            }

            auto &entry = it->second;
            entry.pass->setCamera(m_camera);
            entry.pass->setEnabled(true);

            const bool willUploadTransforms = batch.uploadTransforms;
            const bool willUploadPoses = batch.uploadPoses;

            if (!willUploadTransforms && !willUploadPoses)
            {
                frameStats.modelBatchesSkipped += 1u;
                continue;
            }

            if (batch.membershipChanged)
                frameStats.modelBatchesMembershipChanged += 1u;

            // Update hashes used for future comparisons.
            entry.lastSubmittedMembershipHash = batch.membershipHash;

            if (willUploadTransforms)
            {
                entry.lastSubmittedTransformHash = batch.transformHash;
                frameStats.transformUploadsInstances += batch.instanceCount;
                frameStats.modelBatchesTransformUploaded += 1u;

                // Upload all instance worlds (no slot allocator yet).
                entry.pass->setInstances(batch.instanceWorlds.data(), batch.instanceCount);
            }

            if (willUploadPoses)
            {
                entry.lastSubmittedPoseHash = batch.poseHash;
                frameStats.poseUploadsInstances += batch.instanceCount;
                frameStats.modelBatchesPoseUploaded += 1u;

                // Upload all palettes (no slot allocator yet).
                entry.pass->setNodePalette(batch.nodePalette.data(), batch.instanceCount, batch.nodeCount);
                if (batch.jointCount > 0 && batch.jointPalette.size() == static_cast<size_t>(batch.instanceCount) * static_cast<size_t>(batch.jointCount))
                    entry.pass->setJointPalette(batch.jointPalette.data(), batch.instanceCount, batch.jointCount);
            }

            // Update per-entity upload states for visible entities in this batch.
            // Since uploads are whole-buffer, treat all entities in the batch as uploaded for that domain.
            if (willUploadTransforms || willUploadPoses)
            {
                auto itBucket = m_visibleBuckets->byModel.find(key);
                if (itBucket != m_visibleBuckets->byModel.end() && itBucket->second.lastUsedFrame == m_visibleBuckets->frame)
                {
                    for (const Engine::ECS::VisibleRenderRef &ref : itBucket->second.refs)
                    {
                        const uint64_t eKey = entityKey(ref.entity);
                        auto itState = m_uploadStates.find(eKey);
                        if (itState == m_uploadStates.end())
                            continue;
                        if (willUploadTransforms)
                            itState->second.lastUploadedTransformVersion = ref.transformVersion;
                        if (willUploadPoses)
                            itState->second.lastUploadedPoseVersion = ref.poseVersion;
                        if (ref.visibleFrame != 0)
                            itState->second.lastUploadedVisibleFrame = ref.visibleFrame;
                    }
                }
            }
        }

        // Disable passes that have no instances this frame.
        for (auto &kv : m_passes)
        {
            const auto it = m_batchesByModel.find(kv.first);
            if (it == m_batchesByModel.end() || it->second.lastUsedFrame != m_frameCounter)
            {
                kv.second.pass->setEnabled(false);
            }
        }

#if !defined(ENGINE_PRODUCTION) || !ENGINE_PRODUCTION
        if ((m_frameCounter % 120u) == 0u)
        {
            const auto tSubmit1 = std::chrono::high_resolution_clock::now();
            const double buildMs = std::chrono::duration<double, std::milli>(tBuild1 - tBuild0).count();
            const double submitMs = std::chrono::duration<double, std::milli>(tSubmit1 - tBuild1).count();
            std::cout << "[RenderSystem] frame=" << m_frameCounter
                      << " candidates=" << frameStats.totalCandidates
                      << " visible=" << frameStats.visibleProcessed
                      << " visibleBatches=" << frameStats.visibleModelBatches
                      << " xformUploads=" << frameStats.transformUploadsInstances << "/" << frameStats.visibleProcessed
                      << " poseUploads=" << frameStats.poseUploadsInstances << "/" << frameStats.visibleProcessed
                      << " batchesSkipped=" << frameStats.modelBatchesSkipped
                      << " membershipChanged=" << frameStats.modelBatchesMembershipChanged
                      << " buildMs=" << buildMs
                      << " submitMs=" << submitMs
                      << "\n";
        }
#endif

        // Occasionally prune upload state for entities that haven't been seen recently.
        if ((m_frameCounter % 600u) == 0u && !m_uploadStates.empty())
        {
            const uint32_t pruneBefore = (m_frameCounter > 600u) ? (m_frameCounter - 600u) : 0u;
            for (auto it = m_uploadStates.begin(); it != m_uploadStates.end();)
            {
                if (it->second.lastSeenFrame < pruneBefore)
                    it = m_uploadStates.erase(it);
                else
                    ++it;
            }
        }
    }

private:
    struct Stats
    {
        uint32_t totalCandidates = 0;
        uint32_t visibleProcessed = 0;

        uint32_t visibleModelBatches = 0;

        // Upload counters (instance counts)
        uint32_t transformUploadsInstances = 0;
        uint32_t poseUploadsInstances = 0;

        // Batch counters
        uint32_t modelBatchesSkipped = 0;
        uint32_t modelBatchesTransformUploaded = 0;
        uint32_t modelBatchesPoseUploaded = 0;
        uint32_t modelBatchesMembershipChanged = 0;
    };

    static constexpr uint32_t kInvalidVersion = std::numeric_limits<uint32_t>::max();

    struct PerEntityUploadState
    {
        uint32_t lastUploadedTransformVersion = kInvalidVersion;
        uint32_t lastUploadedPoseVersion = kInvalidVersion;
        uint32_t lastUploadedVisibleFrame = 0;
        uint32_t lastSeenFrame = 0;
    };

    struct PerModelBatch
    {
        Engine::ModelHandle handle{};
        uint32_t lastUsedFrame = 0;

        uint32_t instanceCount = 0;

        uint64_t membershipHash = 0;
        uint64_t transformHash = 0;
        uint64_t poseHash = 0;

        bool membershipChanged = false;
        bool anyTransformDirty = false;
        bool anyPoseDirty = false;
        bool uploadTransforms = false;
        bool uploadPoses = false;

        uint32_t dirtyTransformEntities = 0;
        uint32_t dirtyPoseEntities = 0;
        uint32_t newlyVisibleEntities = 0;

        std::vector<glm::mat4> instanceWorlds;
        std::vector<glm::mat4> nodePalette; // flattened: [instance][node]
        uint32_t nodeCount = 0;

        std::vector<glm::mat4> jointPalette; // flattened: [instance][joint]
        uint32_t jointCount = 0;
    };

    struct PassEntry
    {
        std::shared_ptr<Engine::SModelRenderPassModule> pass;
        uint64_t lastSubmittedMembershipHash = 0;
        uint64_t lastSubmittedTransformHash = 0;
        uint64_t lastSubmittedPoseHash = 0;
    };

    Engine::AssetManager *m_assets = nullptr; // not owned
    Engine::Renderer *m_renderer = nullptr;   // not owned
    Engine::Camera *m_camera = nullptr;       // not owned
    const Engine::ECS::VisibleRenderBuckets *m_visibleBuckets = nullptr; // not owned

    std::unordered_map<uint64_t, PassEntry> m_passes;
    std::unordered_map<uint64_t, PerModelBatch> m_batchesByModel;
    std::unordered_map<uint64_t, PerEntityUploadState> m_uploadStates;
    uint32_t m_frameCounter = 0;
};
