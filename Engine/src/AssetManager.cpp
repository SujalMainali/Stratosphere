#include "assets/AssetManager.h"
#include "utils/ImageUtils.h" // UploadContext

#include <utility>
#include <cstring>

namespace Engine
{
    // ------------------------------------------------------------
    // Helpers: map smodel enum ints -> Vulkan settings
    // ------------------------------------------------------------
    static VkSamplerAddressMode toVkWrap(uint32_t wrap)
    {
        // Your .smodel uses: 0=Repeat,1=Clamp,2=Mirror
        switch (wrap)
        {
        default:
        case 0:
            return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 1:
            return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 2:
            return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
    }

    static VkFilter toVkFilter(uint32_t f)
    {
        // 0=Nearest,1=Linear
        switch (f)
        {
        default:
        case 0:
            return VK_FILTER_NEAREST;
        case 1:
            return VK_FILTER_LINEAR;
        }
    }

    static VkSamplerMipmapMode toVkMip(uint32_t m)
    {
        // 0=None,1=Nearest,2=Linear
        switch (m)
        {
        default:
        case 0:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST; // no mip levels anyway (phase 1)
        case 1:
            return VK_SAMPLER_MIPMAP_MODE_NEAREST;
        case 2:
            return VK_SAMPLER_MIPMAP_MODE_LINEAR;
        }
    }

    // ------------------------------------------------------------
    // AssetManager
    // ------------------------------------------------------------
    AssetManager::AssetManager(VkDevice device,
                               VkPhysicalDevice phys,
                               VkQueue graphicsQueue,
                               uint32_t graphicsQueueFamilyIndex)
        : m_device(device),
          m_phys(phys),
          m_graphicsQueue(graphicsQueue),
          m_graphicsQueueFamilyIndex(graphicsQueueFamilyIndex)
    {
    }

    AssetManager::~AssetManager()
    {
        // Destroy meshes
        for (auto &kv : m_meshes)
        {
            if (kv.second.asset)
                kv.second.asset->destroy(m_device);
        }

        // Destroy textures
        for (auto &kv : m_textures)
        {
            if (kv.second.asset)
                kv.second.asset->destroy(m_device);
        }

        // Materials + Models are CPU only (no gpu destroy needed)
        m_meshes.clear();
        m_textures.clear();
        m_materials.clear();
        m_models.clear();

        m_meshPathCache.clear();
        m_modelPathCache.clear();
    }

    // ------------------------------------------------------------
    // Mesh existing API
    // ------------------------------------------------------------
    MeshHandle AssetManager::loadMesh(const std::string &cookedMeshPath)
    {
        auto it = m_meshPathCache.find(cookedMeshPath);
        if (it != m_meshPathCache.end())
        {
            addRef(it->second);
            return it->second;
        }

        MeshData data;
        if (!LoadSMeshV0FromFile(cookedMeshPath, data))
            return MeshHandle{};

        MeshHandle h = createMeshFromData_Internal(data, cookedMeshPath, 1);
        if (h.isValid())
            m_meshPathCache.emplace(cookedMeshPath, h);

        return h;
    }

    MeshAsset *AssetManager::getMesh(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it == m_meshes.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it != m_meshes.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(MeshHandle h)
    {
        auto it = m_meshes.find(h.id);
        if (it != m_meshes.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    MeshHandle AssetManager::createMeshFromData_Internal(const MeshData &data, const std::string &path, uint32_t initialRef)
    {
        // Transient pool per mesh upload
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool);
        if (pr != VK_SUCCESS)
            return MeshHandle{};

        auto asset = std::make_unique<MeshAsset>();
        const bool ok = asset->upload(m_device, m_phys, uploadPool, m_graphicsQueue, data);

        vkDestroyCommandPool(m_device, uploadPool, nullptr);

        if (!ok)
            return MeshHandle{};

        const uint64_t id = m_nextMeshID++;
        MeshEntry entry;
        entry.asset = std::move(asset);
        entry.generation = 1;
        entry.refCount = initialRef;
        entry.path = path;

        m_meshes.emplace(id, std::move(entry));

        MeshHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    // ------------------------------------------------------------
    // Texture API
    // ------------------------------------------------------------
    TextureHandle AssetManager::createTexture_Internal(std::unique_ptr<TextureAsset> tex, uint32_t initialRef)
    {
        const uint64_t id = m_nextTextureID++;
        TextureEntry e;
        e.asset = std::move(tex);
        e.generation = 1;
        e.refCount = initialRef;

        m_textures.emplace(id, std::move(e));

        TextureHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    TextureAsset *AssetManager::getTexture(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it == m_textures.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(TextureHandle h)
    {
        auto it = m_textures.find(h.id);
        if (it != m_textures.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Material API
    // ------------------------------------------------------------
    MaterialHandle AssetManager::createMaterial_Internal(std::unique_ptr<MaterialAsset> mat, uint32_t initialRef)
    {
        const uint64_t id = m_nextMaterialID++;
        MaterialEntry e;
        e.asset = std::move(mat);
        e.generation = 1;
        e.refCount = initialRef;

        // Gather dependency handles (textures)
        if (e.asset)
        {
            if (e.asset->baseColorTexture.isValid())
                e.textureDeps.push_back(e.asset->baseColorTexture);
            if (e.asset->normalTexture.isValid())
                e.textureDeps.push_back(e.asset->normalTexture);
            if (e.asset->metallicRoughnessTexture.isValid())
                e.textureDeps.push_back(e.asset->metallicRoughnessTexture);
            if (e.asset->occlusionTexture.isValid())
                e.textureDeps.push_back(e.asset->occlusionTexture);
            if (e.asset->emissiveTexture.isValid())
                e.textureDeps.push_back(e.asset->emissiveTexture);
        }

        m_materials.emplace(id, std::move(e));

        MaterialHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    MaterialAsset *AssetManager::getMaterial(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it == m_materials.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it != m_materials.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(MaterialHandle h)
    {
        auto it = m_materials.find(h.id);
        if (it != m_materials.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Model API
    // ------------------------------------------------------------
    ModelHandle AssetManager::createModel_Internal(std::unique_ptr<ModelAsset> model, const std::string &path, uint32_t initialRef)
    {
        const uint64_t id = m_nextModelID++;
        ModelEntry e;
        e.asset = std::move(model);
        e.generation = 1;
        e.refCount = initialRef;
        e.path = path;

        // Dependencies (fill later in loadModel)
        m_models.emplace(id, std::move(e));

        ModelHandle h;
        h.id = id;
        h.generation = 1;
        return h;
    }

    ModelHandle AssetManager::loadModel(const std::string &cookedModelPath)
    {
        auto it = m_modelPathCache.find(cookedModelPath);
        if (it != m_modelPathCache.end())
        {
            addRef(it->second);
            return it->second;
        }

        // --------------------------
        // Parse cooked .smodel file
        // --------------------------
        Engine::smodel::SModelFileView view;
        std::string err;
        if (!Engine::smodel::LoadSModelFile(cookedModelPath, view, err))
        {
            return ModelHandle{};
        }

        // --------------------------
        // Create upload pool for all textures (single submit)
        // --------------------------
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.queueFamilyIndex = m_graphicsQueueFamilyIndex;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        VkResult pr = vkCreateCommandPool(m_device, &poolInfo, nullptr, &uploadPool);
        if (pr != VK_SUCCESS)
            return ModelHandle{};

        Engine::UploadContext upload{};
        if (!Engine::BeginUploadContext(upload, m_device, m_phys, uploadPool, m_graphicsQueue))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return ModelHandle{};
        }

        // --------------------------
        // Upload textures (deferred)
        // --------------------------
        std::vector<TextureHandle> textureHandles;
        textureHandles.resize(view.textureCount());

        for (uint32_t i = 0; i < view.textureCount(); i++)
        {
            const auto &t = view.textures[i];

            // These fields come from your .smodel texture record format
            const bool isSRGB = (t.colorSpace == 1); // 1 = SRGB
            const VkSamplerAddressMode wrapU = toVkWrap(t.wrapU);
            const VkSamplerAddressMode wrapV = toVkWrap(t.wrapV);
            const VkFilter minF = toVkFilter(t.minFilter);
            const VkFilter magF = toVkFilter(t.magFilter);
            const VkSamplerMipmapMode mipM = toVkMip(t.mipFilter);

            const uint8_t *bytes = view.blob + t.imageDataOffset;
            const size_t sizeBytes = static_cast<size_t>(t.imageDataSize);

            auto tex = std::make_unique<TextureAsset>();

            // IMPORTANT:
            // We create textures with refCount=0 (materials will addRef them)
            // This avoids leaking textures when model is destroyed.
            if (!tex->uploadEncodedImage_Deferred(
                    upload,
                    bytes,
                    sizeBytes,
                    isSRGB,
                    wrapU,
                    wrapV,
                    minF,
                    magF,
                    mipM,
                    t.maxAnisotropy))
            {
                // Cleanup on failure
                Engine::EndSubmitAndWait(upload);
                vkDestroyCommandPool(m_device, uploadPool, nullptr);
                return ModelHandle{};
            }

            textureHandles[i] = createTexture_Internal(std::move(tex), 0);
        }

        // ONE SUBMIT for all textures
        if (!Engine::EndSubmitAndWait(upload))
        {
            vkDestroyCommandPool(m_device, uploadPool, nullptr);
            return ModelHandle{};
        }

        vkDestroyCommandPool(m_device, uploadPool, nullptr);

        // --------------------------
        // Create materials (CPU only)
        // Materials addRef() to textures they use
        // --------------------------
        std::vector<MaterialHandle> materialHandles;
        materialHandles.resize(view.materialCount());

        for (uint32_t i = 0; i < view.materialCount(); i++)
        {
            const auto &m = view.materials[i];

            auto mat = std::make_unique<MaterialAsset>();
            mat->debugName = view.getStringOrEmpty(m.nameStrOffset);

            std::memcpy(mat->baseColorFactor, m.baseColorFactor, sizeof(mat->baseColorFactor));
            std::memcpy(mat->emissiveFactor, m.emissiveFactor, sizeof(mat->emissiveFactor));

            mat->metallicFactor = m.metallicFactor;
            mat->roughnessFactor = m.roughnessFactor;

            mat->normalScale = m.normalScale;
            mat->occlusionStrength = m.occlusionStrength;
            mat->alphaCutoff = m.alphaCutoff;

            mat->alphaMode = m.alphaMode;
            mat->doubleSided = m.doubleSided;

            // Convert texture indices -> handles (-1 means none)
            auto grabTex = [&](int32_t idx) -> TextureHandle
            {
                if (idx < 0)
                    return TextureHandle{};
                return textureHandles[static_cast<uint32_t>(idx)];
            };

            mat->baseColorTexture = grabTex(m.baseColorTexture);
            mat->normalTexture = grabTex(m.normalTexture);
            mat->metallicRoughnessTexture = grabTex(m.metallicRoughnessTexture);
            mat->occlusionTexture = grabTex(m.occlusionTexture);
            mat->emissiveTexture = grabTex(m.emissiveTexture);

            mat->baseColorTexCoord = m.baseColorTexCoord;
            mat->normalTexCoord = m.normalTexCoord;
            mat->metallicRoughnessTexCoord = m.metallicRoughnessTexCoord;
            mat->occlusionTexCoord = m.occlusionTexCoord;
            mat->emissiveTexCoord = m.emissiveTexCoord;

            // Create material with refCount=0 (model will addRef materials it uses)
            MaterialHandle mh = createMaterial_Internal(std::move(mat), 0);
            materialHandles[i] = mh;

            // AddRef textures used by this material
            auto *createdMat = getMaterial(mh);
            if (createdMat)
            {
                if (createdMat->baseColorTexture.isValid())
                    addRef(createdMat->baseColorTexture);
                if (createdMat->normalTexture.isValid())
                    addRef(createdMat->normalTexture);
                if (createdMat->metallicRoughnessTexture.isValid())
                    addRef(createdMat->metallicRoughnessTexture);
                if (createdMat->occlusionTexture.isValid())
                    addRef(createdMat->occlusionTexture);
                if (createdMat->emissiveTexture.isValid())
                    addRef(createdMat->emissiveTexture);
            }
        }

        // --------------------------
        // Create meshes (GPU upload)
        // Model will addRef() meshes it uses
        // --------------------------
        std::vector<MeshHandle> meshHandles;
        meshHandles.resize(view.meshCount());

        for (uint32_t i = 0; i < view.meshCount(); i++)
        {
            const auto &mr = view.meshes[i];

            MeshData md;
            md.vertexCount = mr.vertexCount;
            md.indexCount = mr.indexCount;
            md.vertexStride = mr.vertexStride;
            md.indexFormat = (mr.indexType == 0) ? 0 : 1;

            std::memcpy(md.aabbMin, mr.aabbMin, sizeof(md.aabbMin));
            std::memcpy(md.aabbMax, mr.aabbMax, sizeof(md.aabbMax));

            // Copy vertex bytes from blob
            const uint8_t *vb = view.blob + mr.vertexDataOffset;
            md.vertexBytes.assign(vb, vb + mr.vertexDataSize);

            // Copy index bytes
            const uint8_t *ib = view.blob + mr.indexDataOffset;

            if (md.indexFormat == 0)
            {
                md.indices16.resize(md.indexCount);
                std::memcpy(md.indices16.data(), ib, md.indexCount * sizeof(uint16_t));
            }
            else
            {
                md.indices32.resize(md.indexCount);
                std::memcpy(md.indices32.data(), ib, md.indexCount * sizeof(uint32_t));
            }

            // Create mesh with refCount=0 (model will addRef as needed)
            meshHandles[i] = createMeshFromData_Internal(md, cookedModelPath + "#mesh" + std::to_string(i), 0);
        }

        // --------------------------
        // Create model primitives
        // Model addsRef() to mesh/material dependencies
        // --------------------------
        auto model = std::make_unique<ModelAsset>();

        model->debugName = ""; // optional: you can store filename later

        model->primitives.resize(view.primitiveCount());

        std::vector<MeshHandle> meshDeps;
        std::vector<MaterialHandle> matDeps;

        for (uint32_t i = 0; i < view.primitiveCount(); i++)
        {
            const auto &p = view.primitives[i];

            ModelPrimitive prim;
            prim.mesh = meshHandles[p.meshIndex];
            prim.material = materialHandles[p.materialIndex];
            prim.firstIndex = p.firstIndex;
            prim.indexCount = p.indexCount;
            prim.vertexOffset = p.vertexOffset;

            model->primitives[i] = prim;

            // Dependency refs:
            // addRef once per primitive usage (fine for now).
            if (prim.mesh.isValid())
            {
                addRef(prim.mesh);
                meshDeps.push_back(prim.mesh);
            }
            if (prim.material.isValid())
            {
                addRef(prim.material);
                matDeps.push_back(prim.material);
            }
        }

        // Register model and cache it
        ModelHandle modelHandle = createModel_Internal(std::move(model), cookedModelPath, 1);

        // Fill dependency lists inside the ModelEntry
        auto modelIt = m_models.find(modelHandle.id);
        if (modelIt != m_models.end())
        {
            modelIt->second.meshDeps = std::move(meshDeps);
            modelIt->second.materialDeps = std::move(matDeps);
        }

        m_modelPathCache.emplace(cookedModelPath, modelHandle);
        return modelHandle;
    }

    ModelAsset *AssetManager::getModel(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it == m_models.end())
            return nullptr;
        if (it->second.generation != h.generation)
            return nullptr;
        return it->second.asset.get();
    }

    void AssetManager::addRef(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it != m_models.end() && it->second.generation == h.generation)
            it->second.refCount++;
    }

    void AssetManager::release(ModelHandle h)
    {
        auto it = m_models.find(h.id);
        if (it != m_models.end() && it->second.generation == h.generation)
        {
            if (it->second.refCount > 0)
                it->second.refCount--;
        }
    }

    // ------------------------------------------------------------
    // Garbage collection with dependency release
    // ------------------------------------------------------------
    void AssetManager::garbageCollect()
    {
        // 1) Destroy models with refCount == 0
        for (auto it = m_models.begin(); it != m_models.end();)
        {
            if (it->second.refCount == 0)
            {
                // Release model deps
                for (auto &mh : it->second.meshDeps)
                    release(mh);
                for (auto &mat : it->second.materialDeps)
                    release(mat);

                m_modelPathCache.erase(it->second.path);
                it = m_models.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 2) Destroy materials with refCount == 0
        for (auto it = m_materials.begin(); it != m_materials.end();)
        {
            if (it->second.refCount == 0)
            {
                // Release textures referenced by this material
                for (auto &th : it->second.textureDeps)
                    release(th);
                it = m_materials.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 3) Destroy meshes with refCount == 0
        for (auto it = m_meshes.begin(); it != m_meshes.end();)
        {
            if (it->second.refCount == 0)
            {
                if (it->second.asset)
                    it->second.asset->destroy(m_device);

                m_meshPathCache.erase(it->second.path);
                it = m_meshes.erase(it);
            }
            else
            {
                ++it;
            }
        }

        // 4) Destroy textures with refCount == 0
        for (auto it = m_textures.begin(); it != m_textures.end();)
        {
            if (it->second.refCount == 0)
            {
                if (it->second.asset)
                    it->second.asset->destroy(m_device);
                it = m_textures.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace Engine
