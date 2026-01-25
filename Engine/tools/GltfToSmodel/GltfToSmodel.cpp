#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <functional>

// Your engine format header (adjust include path if needed)
#include "assets/ModelFormat.h"

#ifndef AI_MATKEY_GLTF_ALPHACUTOFF
// Older Assimp doesn't expose this macro, but the property exists in glTF materials.
// "$mat.gltf.alphaCutoff" is what Assimp stores internally.
#define AI_MATKEY_GLTF_ALPHACUTOFF "$mat.gltf.alphaCutoff", 0, 0
#endif

#ifndef AI_MATKEY_GLTF_ALPHAMODE
// Alpha mode is typically stored as a string: "OPAQUE", "MASK", "BLEND"
#define AI_MATKEY_GLTF_ALPHAMODE "$mat.gltf.alphaMode", 0, 0
#endif

// ------------------------------------------------------------
// Namespace alias for readability
// ------------------------------------------------------------
namespace sm = Engine::smodel;

// ------------------------------------------------------------
// Small helpers: filesystem + bytes
// ------------------------------------------------------------
static std::string NormalizePathSlashes(std::string s)
{
    for (char &c : s)
        if (c == '\\')
            c = '/';
    return s;
}

static std::string GetDirectoryOfFile(const std::string &filepath)
{
    std::filesystem::path p(filepath);
    if (p.has_parent_path())
        return NormalizePathSlashes(p.parent_path().string());
    return ".";
}

static bool ReadFileBytes(const std::string &path, std::vector<uint8_t> &outBytes)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return false;

    const std::streamsize size = f.tellg();
    if (size <= 0)
        return false;

    f.seekg(0, std::ios::beg);
    outBytes.resize(static_cast<size_t>(size));
    if (!f.read(reinterpret_cast<char *>(outBytes.data()), size))
        return false;

    return true;
}

// ------------------------------------------------------------
// StringTable: offset-based string storage (0 = empty)
// ------------------------------------------------------------
struct StringTable
{
    std::vector<char> data;

    StringTable()
    {
        // offset 0 reserved for empty string
        data.push_back('\0');
    }

    uint32_t add(const std::string &s)
    {
        if (s.empty())
            return 0;

        uint32_t off = static_cast<uint32_t>(data.size());
        data.insert(data.end(), s.begin(), s.end());
        data.push_back('\0');
        return off;
    }
};

// ------------------------------------------------------------
// Blob: stores vertex/index/image bytes
// ------------------------------------------------------------
struct Blob
{
    std::vector<uint8_t> bytes;

    // optional alignment for tidiness (not required, but safe)
    void align(size_t alignment)
    {
        size_t mod = bytes.size() % alignment;
        if (mod != 0)
        {
            size_t pad = alignment - mod;
            bytes.insert(bytes.end(), pad, 0);
        }
    }

    uint64_t append(const void *src, size_t size)
    {
        uint64_t off = static_cast<uint64_t>(bytes.size());
        const uint8_t *b = reinterpret_cast<const uint8_t *>(src);
        bytes.insert(bytes.end(), b, b + size);
        return off;
    }
};

// ------------------------------------------------------------
// Mesh vertex layout (Phase 1 fixed format for renderer)
// Matches what we described in the renderer doc:
//
// location 0: vec3 position
// location 1: vec3 normal
// location 2: vec2 uv0
// location 3: vec4 tangent
// ------------------------------------------------------------
struct VertexPNTT
{
    float pos[3];
    float normal[3];
    float uv0[2];
    float tangent[4];
};
static_assert(sizeof(VertexPNTT) == 48, "VertexPNTT expected to be 48 bytes");

// ------------------------------------------------------------
// AABB compute
// ------------------------------------------------------------
static void ComputeAABB(const std::vector<VertexPNTT> &v, float outMin[3], float outMax[3])
{
    if (v.empty())
    {
        outMin[0] = outMin[1] = outMin[2] = 0.0f;
        outMax[0] = outMax[1] = outMax[2] = 0.0f;
        return;
    }

    outMin[0] = outMax[0] = v[0].pos[0];
    outMin[1] = outMax[1] = v[0].pos[1];
    outMin[2] = outMax[2] = v[0].pos[2];

    for (const auto &vx : v)
    {
        outMin[0] = std::min(outMin[0], vx.pos[0]);
        outMin[1] = std::min(outMin[1], vx.pos[1]);
        outMin[2] = std::min(outMin[2], vx.pos[2]);

        outMax[0] = std::max(outMax[0], vx.pos[0]);
        outMax[1] = std::max(outMax[1], vx.pos[1]);
        outMax[2] = std::max(outMax[2], vx.pos[2]);
    }
}

// ------------------------------------------------------------
// Assimp texture path handling
// glTF .glb embedded textures use "*0", "*1", etc.
// External textures are relative to the gltf file directory.
// ------------------------------------------------------------
static bool IsEmbeddedTexturePath(const std::string &p)
{
    return !p.empty() && p[0] == '*';
}

static int EmbeddedTextureIndex(const std::string &p)
{
    // "*0" -> 0
    if (!IsEmbeddedTexturePath(p))
        return -1;
    return std::atoi(p.c_str() + 1);
}

// ------------------------------------------------------------
// Extract wrap modes from Assimp
// (Assimp provides aiTextureMapMode)
// ------------------------------------------------------------
static uint32_t ConvertWrapMode(aiTextureMapMode m)
{
    // Our .smodel: 0=Repeat,1=Clamp,2=Mirror
    switch (m)
    {
    default:
    case aiTextureMapMode_Wrap:
        return 0;
    case aiTextureMapMode_Clamp:
        return 1;
    case aiTextureMapMode_Mirror:
        return 2;
    }
}

// ------------------------------------------------------------
// Convert from Assimp to our filter enums
// We don't always get exact filters from Assimp for glTF,
// so we default to Linear.
// Our .smodel: filter 0=Nearest,1=Linear
// mip 0=None,1=Nearest,2=Linear
// ------------------------------------------------------------
static uint32_t DefaultFilterLinear() { return 1; }
static uint32_t DefaultMipNone() { return 0; }

// ------------------------------------------------------------
// Robust material texture query helper
// Attempts multiple assimp texture types when needed.
// ------------------------------------------------------------
static bool TryGetTexture(aiMaterial *mat, aiTextureType type, aiString &outPath)
{
    if (!mat)
        return false;
    if (mat->GetTextureCount(type) == 0)
        return false;
    if (AI_SUCCESS != mat->GetTexture(type, 0, &outPath))
        return false;
    if (outPath.length == 0)
        return false;
    return true;
}

// ------------------------------------------------------------
// Build absolute/normalized filesystem path for external textures
// ------------------------------------------------------------
static std::string ResolveTexturePath(const std::string &modelDir, const std::string &rawAssimpPath)
{
    std::string p = NormalizePathSlashes(rawAssimpPath);

    // If already absolute, keep it
    std::filesystem::path fp(p);
    if (fp.is_absolute())
        return NormalizePathSlashes(fp.string());

    // Otherwise treat as relative to model directory
    std::filesystem::path resolved = std::filesystem::path(modelDir) / fp;
    return NormalizePathSlashes(resolved.lexically_normal().string());
}

// ------------------------------------------------------------
// Texture loading (external vs embedded)
// IMPORTANT: runtime uses stb_image to decode bytes -> store compressed bytes
// For embedded textures in glTF, Assimp usually stores compressed bytes (height==0)
// ------------------------------------------------------------
struct LoadedImageBytes
{
    std::vector<uint8_t> bytes; // compressed image bytes (PNG/JPG/etc)
    std::string debugURI;       // for string table (path or "*0")
    bool ok = false;
};

static LoadedImageBytes LoadTextureBytesFromAssimp(
    const aiScene *scene,
    const std::string &modelDir,
    const std::string &assimpPath)
{
    LoadedImageBytes out{};
    out.debugURI = assimpPath;

    // Embedded case: "*0"
    if (IsEmbeddedTexturePath(assimpPath))
    {
        const int idx = EmbeddedTextureIndex(assimpPath);
        if (!scene || idx < 0 || idx >= (int)scene->mNumTextures)
        {
            std::cout << "Embedded texture index invalid: " << assimpPath << "\n";
            return out;
        }

        const aiTexture *tex = scene->mTextures[idx];
        if (!tex)
        {
            std::cout << "Embedded texture missing: " << assimpPath << "\n";
            return out;
        }

        // Most glTF embedded textures are compressed:
        // mHeight == 0 means data blob, mWidth = byte size
        if (tex->mHeight == 0)
        {
            const size_t byteSize = static_cast<size_t>(tex->mWidth);
            out.bytes.resize(byteSize);
            std::memcpy(out.bytes.data(), tex->pcData, byteSize);
            out.ok = true;
            return out;
        }

        // If it's raw (rare for glTF), we cannot store as compressed reliably without encoding.
        // You can add stb_image_write here later if you want to support it.
        std::cout << "WARNING: Embedded texture is raw (mHeight>0). Not supported in phase 1: "
                  << assimpPath << "\n";
        return out;
    }

    // External file
    const std::string resolved = ResolveTexturePath(modelDir, assimpPath);
    if (!ReadFileBytes(resolved, out.bytes))
    {
        std::cout << "Failed to read external texture: " << resolved << "\n";
        return out;
    }

    out.debugURI = resolved; // store resolved path for reference
    out.ok = true;
    return out;
}

// ------------------------------------------------------------
// Packing a single .smodel file:
// V2 layout
// [Header]
// [Meshes]
// [Primitives]
// [Materials]
// [Textures]
// [Nodes]
// [NodePrimitiveIndices]
// [StringTable]
// [Blob]
// ------------------------------------------------------------
template <typename T>
static void WriteVector(std::ofstream &out, const std::vector<T> &v)
{
    if (!v.empty())
        out.write(reinterpret_cast<const char *>(v.data()), sizeof(T) * v.size());
}

static void WriteBytes(std::ofstream &out, const std::vector<uint8_t> &b)
{
    if (!b.empty())
        out.write(reinterpret_cast<const char *>(b.data()), b.size());
}

static void WriteChars(std::ofstream &out, const std::vector<char> &c)
{
    if (!c.empty())
        out.write(reinterpret_cast<const char *>(c.data()), c.size());
}

// ------------------------------------------------------------
// Main
// ------------------------------------------------------------
int main(int argc, char **argv)
{
    if (argc < 3)
    {
        std::cout << "Usage: GltfToSModel <input.gltf/.glb> <output.smodel>\n";
        return 0;
    }

    const std::string inputPath = NormalizePathSlashes(argv[1]);
    const std::string outputPath = NormalizePathSlashes(argv[2]);
    const std::string modelDir = GetDirectoryOfFile(inputPath);

    std::cout << "Input  : " << inputPath << "\n";
    std::cout << "Output : " << outputPath << "\n";
    std::cout << "ModelDir: " << modelDir << "\n";

    // ------------------------------------------------------------
    // Assimp importer options:
    // - Triangulate: ensures triangles
    // - GenNormals: normals exist
    // - CalcTangentSpace: tangents exist (when UV exists)
    // - JoinIdenticalVertices: reduces duplicates
    // ------------------------------------------------------------
    Assimp::Importer importer;

    const unsigned flags =
        aiProcess_Triangulate |
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace |
        aiProcess_JoinIdenticalVertices |
        aiProcess_ImproveCacheLocality |
        aiProcess_LimitBoneWeights |
        aiProcess_RemoveRedundantMaterials |
        aiProcess_SortByPType;

    const aiScene *scene = importer.ReadFile(inputPath, flags);
    if (!scene)
    {
        std::cout << "Assimp failed: " << importer.GetErrorString() << "\n";
        return 1;
    }

    // ------------------------------------------------------------
    // Build tables
    // ------------------------------------------------------------
    StringTable strings;
    Blob blob;

    std::vector<sm::SModelMeshRecord> meshRecords;
    std::vector<sm::SModelPrimitiveRecord> primRecords;
    std::vector<sm::SModelMaterialRecord> materialRecords;
    std::vector<sm::SModelTextureRecord> textureRecords;
    std::vector<sm::SModelNodeRecord> nodeRecords;
    std::vector<uint32_t> nodePrimitiveIndices;

    // ------------------------------------------------------------
    // Texture dedup map
    // key = resolved path or "*0"
    // ------------------------------------------------------------
    std::unordered_map<std::string, int32_t> textureKeyToIndex;

    // Create & store a new texture record (or return existing index)
    auto AcquireTextureIndex = [&](const std::string &assimpTexPath,
                                   bool isSRGB,
                                   aiMaterial *mat,
                                   aiTextureType type) -> int32_t
    {
        if (assimpTexPath.empty())
            return -1;

        // If external: resolve path; if embedded: keep "*0"
        std::string key = assimpTexPath;
        if (!IsEmbeddedTexturePath(key))
            key = ResolveTexturePath(modelDir, key);

        key = NormalizePathSlashes(key);

        auto it = textureKeyToIndex.find(key);
        if (it != textureKeyToIndex.end())
            return it->second;

        // Load bytes
        LoadedImageBytes img = LoadTextureBytesFromAssimp(scene, modelDir, assimpTexPath);
        if (!img.ok)
            return -1;

        // Wrap modes from assimp (if available)
        aiTextureMapMode modeU = aiTextureMapMode_Wrap;
        aiTextureMapMode modeV = aiTextureMapMode_Wrap;
        mat->Get(AI_MATKEY_MAPPINGMODE_U(type, 0), modeU);
        mat->Get(AI_MATKEY_MAPPINGMODE_V(type, 0), modeV);

        sm::SModelTextureRecord tr{};
        tr.nameStrOffset = strings.add(key);
        tr.uriStrOffset = strings.add(img.debugURI);

        tr.colorSpace = isSRGB ? 1 : 0; // 1=SRGB,0=Linear
        tr.encoding = 0;                // 0 = encoded bytes (PNG/JPG/etc)

        tr.wrapU = ConvertWrapMode(modeU);
        tr.wrapV = ConvertWrapMode(modeV);

        tr.minFilter = DefaultFilterLinear();
        tr.magFilter = DefaultFilterLinear();
        tr.mipFilter = DefaultMipNone();
        tr.maxAnisotropy = 1.0f;

        // Blob store (compressed)
        blob.align(8);
        tr.imageDataOffset = blob.append(img.bytes.data(), img.bytes.size());
        tr.imageDataSize = static_cast<uint32_t>(img.bytes.size());

        const int32_t newIndex = static_cast<int32_t>(textureRecords.size());
        textureRecords.push_back(tr);
        textureKeyToIndex[key] = newIndex;
        return newIndex;
    };

    // ------------------------------------------------------------
    // Materials (Assimp materials count includes default materials)
    // We output materialRecords of the same count so mesh->mMaterialIndex can index directly.
    // ------------------------------------------------------------
    materialRecords.resize(scene->mNumMaterials);

    for (uint32_t mi = 0; mi < scene->mNumMaterials; ++mi)
    {
        aiMaterial *mat = scene->mMaterials[mi];
        sm::SModelMaterialRecord mr{};

        // Name
        {
            aiString n;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_NAME, n))
                mr.nameStrOffset = strings.add(n.C_Str());
            else
                mr.nameStrOffset = 0;
        }

        // Defaults
        mr.baseColorFactor[0] = 1.0f;
        mr.baseColorFactor[1] = 1.0f;
        mr.baseColorFactor[2] = 1.0f;
        mr.baseColorFactor[3] = 1.0f;

        mr.emissiveFactor[0] = 0.0f;
        mr.emissiveFactor[1] = 0.0f;
        mr.emissiveFactor[2] = 0.0f;

        mr.metallicFactor = 1.0f;
        mr.roughnessFactor = 1.0f;

        mr.normalScale = 1.0f;
        mr.occlusionStrength = 1.0f;

        mr.alphaMode = 0; // Opaque
        mr.alphaCutoff = 0.5f;
        mr.doubleSided = 0;

        // Texture indices default to "none"
        mr.baseColorTexture = -1;
        mr.normalTexture = -1;
        mr.metallicRoughnessTexture = -1;
        mr.occlusionTexture = -1;
        mr.emissiveTexture = -1;

        mr.baseColorTexCoord = 0;
        mr.normalTexCoord = 0;
        mr.metallicRoughnessTexCoord = 0;
        mr.occlusionTexCoord = 0;
        mr.emissiveTexCoord = 0;

        // Base color factor (glTF PBR)
        {
            aiColor4D c;
            if (AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_BASE_COLOR, &c))
            {
                mr.baseColorFactor[0] = c.r;
                mr.baseColorFactor[1] = c.g;
                mr.baseColorFactor[2] = c.b;
                mr.baseColorFactor[3] = c.a;
            }
        }

        // Emissive factor
        {
            aiColor3D e(0.f, 0.f, 0.f);
            if (AI_SUCCESS == mat->Get(AI_MATKEY_COLOR_EMISSIVE, e))
            {
                mr.emissiveFactor[0] = e.r;
                mr.emissiveFactor[1] = e.g;
                mr.emissiveFactor[2] = e.b;
            }
        }

        // Metallic / roughness
        {
            float metallic = 1.0f;
            float roughness = 1.0f;

            // Assimp glTF often supports these keys
            mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic);
            mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness);

            mr.metallicFactor = metallic;
            mr.roughnessFactor = roughness;
        }

        // Alpha mode / cutoff
        {
            // Many glTF imports store opacity in AI_MATKEY_OPACITY; alphaMode not always present via Assimp.
            // For Phase 1 we keep opaque unless explicitly transparent.
            float opacity = 1.0f;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_OPACITY, opacity))
            {
                if (opacity < 1.0f)
                {
                    mr.alphaMode = 2; // Blend
                }
            }

            // Alpha cutoff (Mask)
            float cutoff = 0.5f;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, cutoff))
            {
                mr.alphaCutoff = cutoff;
                mr.alphaMode = 1; // MASK
            }

            // Alpha mode string if present
            aiString alphaModeStr;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaModeStr))
            {
                std::string mode = alphaModeStr.C_Str();
                if (mode == "OPAQUE")
                    mr.alphaMode = 0;
                else if (mode == "MASK")
                    mr.alphaMode = 1;
                else if (mode == "BLEND")
                    mr.alphaMode = 2;
            }

            // Double sided
            int ds = 0;
            if (AI_SUCCESS == mat->Get(AI_MATKEY_TWOSIDED, ds))
                mr.doubleSided = (uint32_t)(ds != 0);
        }

        // ------------------------------------------------------------
        // Textures: robust extraction
        // ------------------------------------------------------------
        auto AssignTextureIfPresent = [&](aiTextureType type, bool srgb, int32_t &outIndex)
        {
            aiString texPath;
            if (!TryGetTexture(mat, type, texPath))
                return;

            const std::string p = NormalizePathSlashes(texPath.C_Str());
            outIndex = AcquireTextureIndex(p, srgb, mat, type);
        };

        // glTF PBR
        AssignTextureIfPresent(aiTextureType_BASE_COLOR, true, mr.baseColorTexture);
        // Some Assimp versions/materials map baseColor to DIFFUSE.
        if (mr.baseColorTexture < 0)
            AssignTextureIfPresent(aiTextureType_DIFFUSE, true, mr.baseColorTexture);
        AssignTextureIfPresent(aiTextureType_NORMALS, false, mr.normalTexture);

        // Metallic-roughness is not always mapped consistently in Assimp depending on version.
        // We try a couple of candidates.
        {
            aiString p;
            if (TryGetTexture(mat, aiTextureType_METALNESS, p))
            {
                mr.metallicRoughnessTexture = AcquireTextureIndex(NormalizePathSlashes(p.C_Str()), false, mat, aiTextureType_METALNESS);
            }
            else if (TryGetTexture(mat, aiTextureType_DIFFUSE_ROUGHNESS, p))
            {
                mr.metallicRoughnessTexture = AcquireTextureIndex(NormalizePathSlashes(p.C_Str()), false, mat, aiTextureType_DIFFUSE_ROUGHNESS);
            }
        }

        // Occlusion
        AssignTextureIfPresent(aiTextureType_AMBIENT_OCCLUSION, false, mr.occlusionTexture);

        // Emissive
        AssignTextureIfPresent(aiTextureType_EMISSIVE, true, mr.emissiveTexture);

        materialRecords[mi] = mr;
    }

    // ------------------------------------------------------------
    // Meshes + primitives
    // Assimp glTF import generally yields one aiMesh per primitive.
    // We'll create one mesh record per aiMesh and one primitive record referencing it.
    // ------------------------------------------------------------
    meshRecords.reserve(scene->mNumMeshes);
    primRecords.reserve(scene->mNumMeshes);

    std::vector<int32_t> meshIndexToPrimIndex(scene->mNumMeshes, -1);

    for (uint32_t meshIdx = 0; meshIdx < scene->mNumMeshes; ++meshIdx)
    {
        const aiMesh *mesh = scene->mMeshes[meshIdx];
        if (!mesh)
            continue;

        // Build vertex array
        std::vector<VertexPNTT> vertices;
        vertices.resize(mesh->mNumVertices);

        for (uint32_t vi = 0; vi < mesh->mNumVertices; ++vi)
        {
            VertexPNTT v{};

            // Position
            v.pos[0] = mesh->mVertices[vi].x;
            v.pos[1] = mesh->mVertices[vi].y;
            v.pos[2] = mesh->mVertices[vi].z;

            // Normal (GenNormals ensures it exists, but still safe)
            if (mesh->HasNormals())
            {
                v.normal[0] = mesh->mNormals[vi].x;
                v.normal[1] = mesh->mNormals[vi].y;
                v.normal[2] = mesh->mNormals[vi].z;
            }
            else
            {
                v.normal[0] = 0;
                v.normal[1] = 1;
                v.normal[2] = 0;
            }

            // UV0
            if (mesh->HasTextureCoords(0))
            {
                v.uv0[0] = mesh->mTextureCoords[0][vi].x;
                v.uv0[1] = mesh->mTextureCoords[0][vi].y;
            }
            else
            {
                v.uv0[0] = 0;
                v.uv0[1] = 0;
            }

            // Tangent (CalcTangentSpace ensures when UVs exist)
            if (mesh->HasTangentsAndBitangents())
            {
                v.tangent[0] = mesh->mTangents[vi].x;
                v.tangent[1] = mesh->mTangents[vi].y;
                v.tangent[2] = mesh->mTangents[vi].z;
                v.tangent[3] = 1.0f;
            }
            else
            {
                v.tangent[0] = 1;
                v.tangent[1] = 0;
                v.tangent[2] = 0;
                v.tangent[3] = 1;
            }

            vertices[vi] = v;
        }

        // Build index array (triangulated)
        std::vector<uint32_t> indices;
        indices.reserve(mesh->mNumFaces * 3);

        for (uint32_t fi = 0; fi < mesh->mNumFaces; ++fi)
        {
            const aiFace &face = mesh->mFaces[fi];
            if (face.mNumIndices != 3)
                continue;

            indices.push_back(face.mIndices[0]);
            indices.push_back(face.mIndices[1]);
            indices.push_back(face.mIndices[2]);
        }

        // Fill mesh record
        sm::SModelMeshRecord mr{};
        {
            const std::string meshName = (mesh->mName.length > 0) ? std::string(mesh->mName.C_Str()) : ("mesh_" + std::to_string(meshIdx));
            mr.nameStrOffset = strings.add(meshName);
        }

        mr.vertexCount = static_cast<uint32_t>(vertices.size());
        mr.indexCount = static_cast<uint32_t>(indices.size());
        mr.vertexStride = static_cast<uint32_t>(sizeof(VertexPNTT));

        // layout flags should match your enum in ModelFormats.h
        // If your enum differs, update accordingly.
        mr.layoutFlags = sm::VTX_POS | sm::VTX_NORMAL | sm::VTX_UV0 | sm::VTX_TANGENT;

        // Indices are always U32 in phase 1
        mr.indexType = 1; // assume 1=U32 (match your IndexType enum if different)

        // AABB
        ComputeAABB(vertices, mr.aabbMin, mr.aabbMax);

        // Store vertex/index bytes in blob
        blob.align(8);
        mr.vertexDataOffset = blob.append(vertices.data(), vertices.size() * sizeof(VertexPNTT));
        mr.vertexDataSize = static_cast<uint32_t>(vertices.size() * sizeof(VertexPNTT));

        blob.align(8);
        mr.indexDataOffset = blob.append(indices.data(), indices.size() * sizeof(uint32_t));
        mr.indexDataSize = static_cast<uint32_t>(indices.size() * sizeof(uint32_t));

        const uint32_t outMeshIndex = static_cast<uint32_t>(meshRecords.size());
        meshRecords.push_back(mr);

        // Primitive record (one per mesh)
        sm::SModelPrimitiveRecord pr{};
        pr.meshIndex = outMeshIndex;
        pr.materialIndex = static_cast<uint32_t>(mesh->mMaterialIndex);
        pr.firstIndex = 0;
        pr.indexCount = mr.indexCount;
        pr.vertexOffset = 0;

        primRecords.push_back(pr);
        meshIndexToPrimIndex[meshIdx] = static_cast<int32_t>(primRecords.size() - 1);
    }

    // ------------------------------------------------------------
    // Build node graph (DFS)
    const uint32_t U32_MAX = ~0u;

    auto ConvertAiToColumnMajor = [](const aiMatrix4x4 &m, float out[16])
    {
        // aiMatrix4x4 is row-major; write into column-major float array explicitly.
        // Column 0
        out[0] = m.a1;
        out[1] = m.b1;
        out[2] = m.c1;
        out[3] = m.d1;
        // Column 1
        out[4] = m.a2;
        out[5] = m.b2;
        out[6] = m.c2;
        out[7] = m.d2;
        // Column 2
        out[8] = m.a3;
        out[9] = m.b3;
        out[10] = m.c3;
        out[11] = m.d3;
        // Column 3 (translation in m.d1/m.d2/m.d3? Actually aiMatrix uses row-major with last column)
        out[12] = m.a4;
        out[13] = m.b4;
        out[14] = m.c4;
        out[15] = m.d4;
    };

    std::function<void(const aiNode *, uint32_t)> EmitNode = [&](const aiNode *n, uint32_t parentIndex)
    {
        if (!n)
            return;

        sm::SModelNodeRecord rec{};
        rec.nameStrOffset = strings.add(n->mName.length > 0 ? std::string(n->mName.C_Str()) : std::string());
        rec.parentIndex = (parentIndex == U32_MAX) ? U32_MAX : parentIndex;
        rec.firstChild = U32_MAX;
        rec.childCount = 0;
        rec.firstPrimitiveIndex = static_cast<uint32_t>(nodePrimitiveIndices.size());
        rec.primitiveCount = 0;
        ConvertAiToColumnMajor(n->mTransformation, rec.localMatrix);

        const uint32_t thisIndex = static_cast<uint32_t>(nodeRecords.size());
        nodeRecords.push_back(rec);

        // Append primitive indices for meshes under this node
        for (unsigned mi = 0; mi < n->mNumMeshes; ++mi)
        {
            const unsigned aiMeshIdx = n->mMeshes[mi];
            int32_t primIdx = (aiMeshIdx < meshIndexToPrimIndex.size()) ? meshIndexToPrimIndex[aiMeshIdx] : -1;
            if (primIdx >= 0)
            {
                nodePrimitiveIndices.push_back(static_cast<uint32_t>(primIdx));
                nodeRecords[thisIndex].primitiveCount++;
            }
        }

        // Children
        if (n->mNumChildren > 0)
        {
            nodeRecords[thisIndex].firstChild = static_cast<uint32_t>(nodeRecords.size());
            for (unsigned ci = 0; ci < n->mNumChildren; ++ci)
            {
                const uint32_t childIndex = static_cast<uint32_t>(nodeRecords.size());
                EmitNode(n->mChildren[ci], thisIndex);
                (void)childIndex; // not used directly; DFS appends sequentially
                nodeRecords[thisIndex].childCount++;
            }
        }
    };

    EmitNode(scene->mRootNode, U32_MAX);

    // Build header offsets
    // File layout:
    // Header
    // MeshRecords
    // PrimitiveRecords
    // MaterialRecords
    // TextureRecords
    // StringTable
    // Blob
    // ------------------------------------------------------------
    sm::SModelHeader header{};
    header.magic = sm::SMODEL_MAGIC;
    header.versionMajor = 2;
    header.versionMinor = 0;

    header.meshCount = static_cast<uint32_t>(meshRecords.size());
    header.primitiveCount = static_cast<uint32_t>(primRecords.size());
    header.materialCount = static_cast<uint32_t>(materialRecords.size());
    header.textureCount = static_cast<uint32_t>(textureRecords.size());
    header.nodeCount = static_cast<uint32_t>(nodeRecords.size());
    header.nodePrimitiveIndexCount = static_cast<uint32_t>(nodePrimitiveIndices.size());

    uint64_t cursor = sizeof(sm::SModelHeader);

    header.meshesOffset = cursor;
    cursor += uint64_t(meshRecords.size()) * sizeof(sm::SModelMeshRecord);

    header.primitivesOffset = cursor;
    cursor += uint64_t(primRecords.size()) * sizeof(sm::SModelPrimitiveRecord);

    header.materialsOffset = cursor;
    cursor += uint64_t(materialRecords.size()) * sizeof(sm::SModelMaterialRecord);

    header.texturesOffset = cursor;
    cursor += uint64_t(textureRecords.size()) * sizeof(sm::SModelTextureRecord);

    // Nodes
    header.nodesOffset = cursor;
    cursor += uint64_t(nodeRecords.size()) * sizeof(sm::SModelNodeRecord);

    // Node primitive indices
    header.nodePrimitiveIndicesOffset = cursor;
    cursor += uint64_t(nodePrimitiveIndices.size()) * sizeof(uint32_t);

    header.stringTableOffset = cursor;
    header.stringTableSize = static_cast<uint32_t>(strings.data.size());
    cursor += strings.data.size();

    header.blobOffset = cursor;
    header.blobSize = static_cast<uint32_t>(blob.bytes.size());
    cursor += blob.bytes.size();

    header.fileSizeBytes = static_cast<uint32_t>(cursor);

    // ------------------------------------------------------------
    // Write output file
    // ------------------------------------------------------------
    std::filesystem::create_directories(std::filesystem::path(outputPath).parent_path());

    std::ofstream out(outputPath, std::ios::binary);
    if (!out.is_open())
    {
        std::cout << "Failed to open output file: " << outputPath << "\n";
        return 2;
    }

    out.write(reinterpret_cast<const char *>(&header), sizeof(header));
    WriteVector(out, meshRecords);
    WriteVector(out, primRecords);
    WriteVector(out, materialRecords);
    WriteVector(out, textureRecords);
    WriteVector(out, nodeRecords);
    WriteVector(out, nodePrimitiveIndices);
    WriteChars(out, strings.data);
    WriteBytes(out, blob.bytes);

    out.close();

    std::cout << "\nCook complete \n";
    std::cout << "Meshes     : " << header.meshCount << "\n";
    std::cout << "Primitives : " << header.primitiveCount << "\n";
    std::cout << "Materials  : " << header.materialCount << "\n";
    std::cout << "Textures   : " << header.textureCount << "\n";
    std::cout << "Nodes      : " << header.nodeCount << "\n";
    std::cout << "NodePrimIx : " << header.nodePrimitiveIndexCount << "\n";
    std::cout << "StringTable: " << header.stringTableSize << " bytes\n";
    std::cout << "Blob       : " << header.blobSize << " bytes\n";
    std::cout << "FileSize   : " << header.fileSizeBytes << " bytes\n";

    return 0;
}
