#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <memory>
#include <vector>

#include "assets/Handles.h"

#include "assets/MeshFormats.h"
#include "assets/MeshAsset.h"

#include "assets/SModelLoader.h"
#include "assets/ModelFormat.h"

#include "assets/TextureAsset.h"
#include "assets/MaterialAsset.h"
#include "assets/ModelAsset.h"

namespace Engine
{
    // ---------------------------
    // AssetManager
    // ---------------------------
    class AssetManager
    {
    public:
        AssetManager(VkDevice device,
                     VkPhysicalDevice phys,
                     VkQueue graphicsQueue,
                     uint32_t graphicsQueueFamilyIndex);
        ~AssetManager();

        // Existing mesh API
        MeshHandle loadMesh(const std::string &cookedMeshPath);
        MeshAsset *getMesh(MeshHandle h);

        void addRef(MeshHandle h);
        void release(MeshHandle h);

        // New smodel/model API
        ModelHandle loadModel(const std::string &cookedModelPath);
        ModelAsset *getModel(ModelHandle h);

        MaterialAsset *getMaterial(MaterialHandle h);
        TextureAsset *getTexture(TextureHandle h);
        TextureHandle loadTextureFromFile(const std::string &filePath);

        void addRef(ModelHandle h);
        void release(ModelHandle h);

        void addRef(MaterialHandle h);
        void release(MaterialHandle h);

        void addRef(TextureHandle h);
        void release(TextureHandle h);

        // Collect all zero-ref assets (and clear caches)
        void garbageCollect();

    private:
        MeshHandle createMeshFromData_Internal(const MeshData &data, const std::string &path, uint32_t initialRef);
        TextureHandle createTexture_Internal(std::unique_ptr<TextureAsset> tex, uint32_t initialRef);
        MaterialHandle createMaterial_Internal(std::unique_ptr<MaterialAsset> mat, uint32_t initialRef);
        ModelHandle createModel_Internal(std::unique_ptr<ModelAsset> model, const std::string &path, uint32_t initialRef);

    private:
        VkDevice m_device = VK_NULL_HANDLE;
        VkPhysicalDevice m_phys = VK_NULL_HANDLE;
        VkQueue m_graphicsQueue = VK_NULL_HANDLE;
        uint32_t m_graphicsQueueFamilyIndex = 0;

        // Separate ID spaces
        uint64_t m_nextMeshID = 1;
        uint64_t m_nextTextureID = 1;
        uint64_t m_nextMaterialID = 1;
        uint64_t m_nextModelID = 1;

        // ---------------------------
        // Mesh entries
        // ---------------------------
        struct MeshEntry
        {
            std::unique_ptr<MeshAsset> asset;
            uint32_t generation = 1;
            uint32_t refCount = 0;
            std::string path;
        };

        // ---------------------------
        // Texture entries
        // ---------------------------
        struct TextureEntry
        {
            std::unique_ptr<TextureAsset> asset;
            uint32_t generation = 1;
            uint32_t refCount = 0;
        };

        std::unordered_map<uint64_t, TextureEntry> m_textures;

        // ---------------------------
        // Material entries
        // ---------------------------
        struct MaterialEntry
        {
            std::unique_ptr<MaterialAsset> asset;
            uint32_t generation = 1;
            uint32_t refCount = 0;

            // Dependencies: textures referenced by this material
            std::vector<TextureHandle> textureDeps;
        };

        std::unordered_map<uint64_t, MaterialEntry> m_materials;

        // ---------------------------
        // Model entries
        // ---------------------------
        struct ModelEntry
        {
            std::unique_ptr<ModelAsset> asset;
            uint32_t generation = 1;
            uint32_t refCount = 0;
            std::string path;

            // Dependencies: meshes + materials used by this model
            std::vector<MeshHandle> meshDeps;
            std::vector<MaterialHandle> materialDeps;
        };

        std::unordered_map<uint64_t, MeshEntry> m_meshes;
        std::unordered_map<std::string, MeshHandle> m_meshPathCache;

        std::unordered_map<uint64_t, ModelEntry> m_models;
        std::unordered_map<std::string, ModelHandle> m_modelPathCache;
    };

} // namespace Engine