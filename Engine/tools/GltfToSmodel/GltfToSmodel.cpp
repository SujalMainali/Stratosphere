#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>

#include <cstdint>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>
#include <cstring>
#include <algorithm>
#include <functional>
#include <cctype>
#include <regex>

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
struct VertexPNTTJW
{
    float pos[3];
    float normal[3];
    float uv0[2];
    float tangent[4];
    uint16_t joints[4];
    float weights[4];
};
static_assert(sizeof(VertexPNTTJW) == 72, "VertexPNTTJW expected to be 72 bytes");

// ------------------------------------------------------------
// AABB compute
// ------------------------------------------------------------
static void ComputeAABB(const std::vector<VertexPNTTJW> &v, float outMin[3], float outMax[3])
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
// Assimp matrix conversion
// aiMatrix4x4 is row-major; runtime expects column-major float arrays.
// ------------------------------------------------------------
static void AiMatToColumnMajor(const aiMatrix4x4 &m, float out[16])
{
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
    // Column 3
    out[12] = m.a4;
    out[13] = m.b4;
    out[14] = m.c4;
    out[15] = m.d4;
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
// Query which UV channel a texture uses.
// In glTF this corresponds to the textureInfo.texCoord field.
// Assimp exposes it via AI_MATKEY_UVWSRC(textureType, textureIndex).
// ------------------------------------------------------------
static uint32_t GetTextureUVSource(aiMaterial *mat, aiTextureType type, uint32_t defaultUv = 0)
{
    if (!mat)
        return defaultUv;

    int uvsrc = static_cast<int>(defaultUv);
    if (AI_SUCCESS == mat->Get(AI_MATKEY_UVWSRC(type, 0), uvsrc))
    {
        if (uvsrc >= 0)
            return static_cast<uint32_t>(uvsrc);
    }
    return defaultUv;
}

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

// ------------------------------------------------------------
// Anim helpers
// ------------------------------------------------------------
static inline float TicksToSeconds(double ticks, double ticksPerSecond)
{
    // Assimp sometimes sets ticksPerSecond = 0
    double tps = (ticksPerSecond != 0.0) ? ticksPerSecond : 1.0;
    return (float)(ticks / tps);
}

static uint16_t AddVec3Sampler(const aiVectorKey *keys,
                               uint32_t keyCount,
                               double tps,
                               sm::SModelAnimInterpolation interp,
                               std::vector<float> &animTimes,
                               std::vector<float> &animValues,
                               std::vector<sm::SModelAnimationSamplerRecord> &animSamplers)
{
    sm::SModelAnimationSamplerRecord s{};
    s.firstTime = (uint32_t)animTimes.size();
    s.timeCount = keyCount;
    s.firstValue = (uint32_t)animValues.size();
    s.valueCount = keyCount * 3;
    s.interpolation = (uint8_t)interp;
    s.valueType = (uint8_t)sm::SModelAnimValueType::Vec3;

    for (uint32_t i = 0; i < keyCount; i++)
    {
        animTimes.push_back(TicksToSeconds(keys[i].mTime, tps));
        animValues.push_back((float)keys[i].mValue.x);
        animValues.push_back((float)keys[i].mValue.y);
        animValues.push_back((float)keys[i].mValue.z);
    }

    uint32_t samplerIndex = (uint32_t)animSamplers.size();
    animSamplers.push_back(s);
    return (uint16_t)samplerIndex;
}

static uint16_t AddQuatSampler(const aiQuatKey *keys,
                               uint32_t keyCount,
                               double tps,
                               sm::SModelAnimInterpolation interp,
                               std::vector<float> &animTimes,
                               std::vector<float> &animValues,
                               std::vector<sm::SModelAnimationSamplerRecord> &animSamplers)
{
    sm::SModelAnimationSamplerRecord s{};
    s.firstTime = (uint32_t)animTimes.size();
    s.timeCount = keyCount;
    s.firstValue = (uint32_t)animValues.size();
    s.valueCount = keyCount * 4;
    s.interpolation = (uint8_t)interp;
    s.valueType = (uint8_t)sm::SModelAnimValueType::Quat;

    for (uint32_t i = 0; i < keyCount; i++)
    {
        animTimes.push_back(TicksToSeconds(keys[i].mTime, tps));
        // Store quat as XYZW (consistent with loader validation expectations)
        animValues.push_back((float)keys[i].mValue.x);
        animValues.push_back((float)keys[i].mValue.y);
        animValues.push_back((float)keys[i].mValue.z);
        animValues.push_back((float)keys[i].mValue.w);
    }

    uint32_t samplerIndex = (uint32_t)animSamplers.size();
    animSamplers.push_back(s);
    return (uint16_t)samplerIndex;
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

static std::string ToLowerASCII(std::string s)
{
    for (char &ch : s)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

static const char *InferAnimTag(const std::string &clipName)
{
    const std::string n = ToLowerASCII(clipName);
    auto has = [&](const char *needle)
    {
        return n.find(needle) != std::string::npos;
    };

    if (has("idle"))
        return "idle";
    if (has("walk"))
        return "walk";
    if (has("run"))
        return "run";
    if (has("jump"))
        return "jump";
    if (has("attack") || has("atk") || has("slash") || has("strike"))
        return "attack";
    if (has("hit") || has("hurt") || has("damage"))
        return "hit";
    if (has("death") || has("die"))
        return "death";
    if (has("cast") || has("spell"))
        return "cast";
    if (has("equip") || has("draw") || has("sheath"))
        return "equip";
    if (has("taunt") || has("emote"))
        return "emote";

    return "unknown";
}

static bool ReadTextFile(const std::string &path, std::string &out)
{
    std::ifstream f(path);
    if (!f.is_open())
        return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

static std::string ReplaceExtension(const std::string &path, const std::string &newExt)
{
    std::filesystem::path p(path);
    p.replace_extension(newExt);
    return NormalizePathSlashes(p.string());
}

static uint32_t FindKeyInterval(const float *times, uint32_t count, float t)
{
    if (count <= 1)
        return 0;
    if (t <= times[0])
        return 0;
    if (t >= times[count - 2])
        return count - 2;

    uint32_t lo = 0, hi = count - 1;
    while (hi - lo > 1)
    {
        uint32_t mid = (lo + hi) / 2;
        if (times[mid] <= t)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static float ComputeAlpha(float t0, float t1, float t)
{
    const float dt = t1 - t0;
    if (dt <= 1e-8f)
        return 0.0f;
    float a = (t - t0) / dt;
    if (a < 0.0f)
        a = 0.0f;
    if (a > 1.0f)
        a = 1.0f;
    return a;
}

static void SampleVec3At(const float *times, const float *values, uint32_t keyCount, float t, float out[3])
{
    if (keyCount == 0)
    {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    if (keyCount == 1)
    {
        out[0] = values[0];
        out[1] = values[1];
        out[2] = values[2];
        return;
    }

    const uint32_t i = FindKeyInterval(times, keyCount, t);
    const float t0 = times[i];
    const float t1 = times[i + 1];
    const float a = ComputeAlpha(t0, t1, t);

    const float *v0 = values + i * 3;
    const float *v1 = values + (i + 1) * 3;
    out[0] = v0[0] + (v1[0] - v0[0]) * a;
    out[1] = v0[1] + (v1[1] - v0[1]) * a;
    out[2] = v0[2] + (v1[2] - v0[2]) * a;
}

static void NormalizeQuat(float q[4])
{
    const float len2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (len2 <= 1e-12f)
    {
        q[0] = q[1] = q[2] = 0.0f;
        q[3] = 1.0f;
        return;
    }
    const float inv = 1.0f / std::sqrt(len2);
    q[0] *= inv;
    q[1] *= inv;
    q[2] *= inv;
    q[3] *= inv;
}

static float DotQuat(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void SlerpQuat(const float q0in[4], const float q1in[4], float a, float out[4])
{
    float q0[4] = {q0in[0], q0in[1], q0in[2], q0in[3]};
    float q1[4] = {q1in[0], q1in[1], q1in[2], q1in[3]};
    NormalizeQuat(q0);
    NormalizeQuat(q1);

    float dot = DotQuat(q0, q1);
    if (dot < 0.0f)
    {
        dot = -dot;
        q1[0] = -q1[0];
        q1[1] = -q1[1];
        q1[2] = -q1[2];
        q1[3] = -q1[3];
    }

    // If very close, lerp to avoid precision issues.
    if (dot > 0.9995f)
    {
        out[0] = q0[0] + (q1[0] - q0[0]) * a;
        out[1] = q0[1] + (q1[1] - q0[1]) * a;
        out[2] = q0[2] + (q1[2] - q0[2]) * a;
        out[3] = q0[3] + (q1[3] - q0[3]) * a;
        NormalizeQuat(out);
        return;
    }

    const float theta = std::acos(std::max(-1.0f, std::min(1.0f, dot)));
    const float s = std::sin(theta);
    const float w0 = std::sin((1.0f - a) * theta) / s;
    const float w1 = std::sin(a * theta) / s;

    out[0] = q0[0] * w0 + q1[0] * w1;
    out[1] = q0[1] * w0 + q1[1] * w1;
    out[2] = q0[2] * w0 + q1[2] * w1;
    out[3] = q0[3] * w0 + q1[3] * w1;
    NormalizeQuat(out);
}

static void SampleQuatAt(const float *times, const float *values, uint32_t keyCount, float t, float out[4])
{
    if (keyCount == 0)
    {
        out[0] = out[1] = out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    if (keyCount == 1)
    {
        out[0] = values[0];
        out[1] = values[1];
        out[2] = values[2];
        out[3] = values[3];
        NormalizeQuat(out);
        return;
    }

    const uint32_t i = FindKeyInterval(times, keyCount, t);
    const float t0 = times[i];
    const float t1 = times[i + 1];
    const float a = ComputeAlpha(t0, t1, t);
    const float *q0 = values + i * 4;
    const float *q1 = values + (i + 1) * 4;
    SlerpQuat(q0, q1, a, out);
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
        aiProcess_FlipUVs |
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
    std::vector<uint32_t> nodeChildIndices;

    // Animations (V3)
    std::vector<sm::SModelAnimationClipRecord> animClips;
    std::vector<sm::SModelAnimationChannelRecord> animChannels;
    std::vector<sm::SModelAnimationSamplerRecord> animSamplers;
    std::vector<float> animTimes;  // seconds
    std::vector<float> animValues; // packed floats (vec3/quats)

    // Skinning (V4)
    struct TmpSkin
    {
        std::string name;
        std::vector<std::string> jointNodeNames; // resolved to node indices after node table is built
        std::vector<float> inverseBindMatrices;  // mat4 array packed as floats (16 floats per joint)
    };

    std::vector<TmpSkin> tmpSkins;
    std::vector<sm::SModelSkinRecord> skinRecords;
    std::vector<uint32_t> skinJointNodeIndices;
    std::vector<float> skinInverseBindMatrices;

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
        auto AssignTextureIfPresent = [&](aiTextureType type, bool srgb, int32_t &outIndex, uint32_t &outTexCoord)
        {
            aiString texPath;
            if (!TryGetTexture(mat, type, texPath))
                return;

            const std::string p = NormalizePathSlashes(texPath.C_Str());
            outIndex = AcquireTextureIndex(p, srgb, mat, type);
            outTexCoord = GetTextureUVSource(mat, type, outTexCoord);
        };

        // glTF PBR
        AssignTextureIfPresent(aiTextureType_BASE_COLOR, true, mr.baseColorTexture, mr.baseColorTexCoord);
        // Some Assimp versions/materials map baseColor to DIFFUSE.
        if (mr.baseColorTexture < 0)
            AssignTextureIfPresent(aiTextureType_DIFFUSE, true, mr.baseColorTexture, mr.baseColorTexCoord);

        AssignTextureIfPresent(aiTextureType_NORMALS, false, mr.normalTexture, mr.normalTexCoord);

        // Metallic-roughness is not always mapped consistently in Assimp depending on version.
        // We try a couple of candidates.
        {
            aiString p;
            if (TryGetTexture(mat, aiTextureType_METALNESS, p))
            {
                mr.metallicRoughnessTexture = AcquireTextureIndex(NormalizePathSlashes(p.C_Str()), false, mat, aiTextureType_METALNESS);
                mr.metallicRoughnessTexCoord = GetTextureUVSource(mat, aiTextureType_METALNESS, mr.metallicRoughnessTexCoord);
            }
            else if (TryGetTexture(mat, aiTextureType_DIFFUSE_ROUGHNESS, p))
            {
                mr.metallicRoughnessTexture = AcquireTextureIndex(NormalizePathSlashes(p.C_Str()), false, mat, aiTextureType_DIFFUSE_ROUGHNESS);
                mr.metallicRoughnessTexCoord = GetTextureUVSource(mat, aiTextureType_DIFFUSE_ROUGHNESS, mr.metallicRoughnessTexCoord);
            }
        }

        // Occlusion
        AssignTextureIfPresent(aiTextureType_AMBIENT_OCCLUSION, false, mr.occlusionTexture, mr.occlusionTexCoord);

        // Emissive
        AssignTextureIfPresent(aiTextureType_EMISSIVE, true, mr.emissiveTexture, mr.emissiveTexCoord);

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

        // We only store a single UV set in the fixed VertexPNTTJW format.
        // Bake the material's selected baseColor texCoord set into uv0.
        uint32_t uvChannel = 0;
        if (mesh->mMaterialIndex < materialRecords.size())
            uvChannel = materialRecords[mesh->mMaterialIndex].baseColorTexCoord;

        int32_t skinIndex = -1;

        // If this mesh has bones, build per-vertex joint/weight data and a TmpSkin.
        std::vector<std::vector<std::pair<uint16_t, float>>> influences; // per vertex: (jointIx, weight)
        if (mesh->HasBones() && mesh->mNumBones > 0)
        {
            TmpSkin skin;
            skin.name = (mesh->mName.length > 0) ? std::string(mesh->mName.C_Str()) + "_skin" : ("skin_" + std::to_string(meshIdx));
            skin.jointNodeNames.reserve(mesh->mNumBones);
            skin.inverseBindMatrices.reserve(size_t(mesh->mNumBones) * 16u);

            influences.resize(mesh->mNumVertices);

            // bone name -> joint index in this skin
            std::unordered_map<std::string, uint16_t> boneNameToJoint;
            boneNameToJoint.reserve(mesh->mNumBones);

            for (uint32_t bi = 0; bi < mesh->mNumBones; ++bi)
            {
                const aiBone *bone = mesh->mBones[bi];
                if (!bone)
                    continue;

                const std::string boneName = (bone->mName.length > 0) ? std::string(bone->mName.C_Str()) : ("bone_" + std::to_string(bi));

                uint16_t jointIx = 0;
                auto it = boneNameToJoint.find(boneName);
                if (it == boneNameToJoint.end())
                {
                    jointIx = static_cast<uint16_t>(skin.jointNodeNames.size());
                    boneNameToJoint.emplace(boneName, jointIx);
                    skin.jointNodeNames.push_back(boneName);

                    float ibm[16];
                    AiMatToColumnMajor(bone->mOffsetMatrix, ibm);
                    skin.inverseBindMatrices.insert(skin.inverseBindMatrices.end(), ibm, ibm + 16);
                }
                else
                {
                    jointIx = it->second;
                }

                for (uint32_t wi = 0; wi < bone->mNumWeights; ++wi)
                {
                    const aiVertexWeight &w = bone->mWeights[wi];
                    if (w.mVertexId >= mesh->mNumVertices)
                        continue;
                    if (w.mWeight <= 0.0f)
                        continue;

                    influences[w.mVertexId].push_back({jointIx, w.mWeight});
                }
            }

            skinIndex = static_cast<int32_t>(tmpSkins.size());
            tmpSkins.push_back(std::move(skin));
        }

        // Build vertex array
        std::vector<VertexPNTTJW> vertices;
        vertices.resize(mesh->mNumVertices);

        for (uint32_t vi = 0; vi < mesh->mNumVertices; ++vi)
        {
            VertexPNTTJW v{};

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
            if (mesh->HasTextureCoords(uvChannel))
            {
                v.uv0[0] = mesh->mTextureCoords[uvChannel][vi].x;
                v.uv0[1] = mesh->mTextureCoords[uvChannel][vi].y;
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

            // Skinning (up to 4 weights)
            if (skinIndex >= 0 && vi < influences.size())
            {
                auto &inf = influences[vi];
                if (!inf.empty())
                {
                    std::sort(inf.begin(), inf.end(), [](const auto &a, const auto &b)
                              { return a.second > b.second; });

                    float sum = 0.0f;
                    const uint32_t take = std::min<uint32_t>(4u, static_cast<uint32_t>(inf.size()));
                    for (uint32_t i = 0; i < take; ++i)
                    {
                        v.joints[i] = inf[i].first;
                        v.weights[i] = inf[i].second;
                        sum += inf[i].second;
                    }

                    if (sum > 0.0f)
                    {
                        for (uint32_t i = 0; i < take; ++i)
                            v.weights[i] /= sum;
                    }
                }
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
        mr.vertexStride = static_cast<uint32_t>(sizeof(VertexPNTTJW));

        // layout flags should match your enum in ModelFormats.h
        // If your enum differs, update accordingly.
        mr.layoutFlags = sm::VTX_POS | sm::VTX_NORMAL | sm::VTX_UV0 | sm::VTX_TANGENT | sm::VTX_JOINTS | sm::VTX_WEIGHTS;

        // Indices are always U32 in phase 1
        mr.indexType = 1; // assume 1=U32 (match your IndexType enum if different)

        // AABB
        ComputeAABB(vertices, mr.aabbMin, mr.aabbMax);

        // Store vertex/index bytes in blob
        blob.align(8);
        mr.vertexDataOffset = blob.append(vertices.data(), vertices.size() * sizeof(VertexPNTTJW));
        mr.vertexDataSize = static_cast<uint32_t>(vertices.size() * sizeof(VertexPNTTJW));

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
        pr.skinIndex = skinIndex;

        primRecords.push_back(pr);
        meshIndexToPrimIndex[meshIdx] = static_cast<int32_t>(primRecords.size() - 1);
    }

    // ------------------------------------------------------------
    // Build node graph (DFS)
    const uint32_t U32_MAX = ~0u;

    // Assimp animation channels target nodes by name
    std::unordered_map<std::string, uint32_t> nodeNameToIndex;

    std::function<uint32_t(const aiNode *, uint32_t)> EmitNode = [&](const aiNode *n, uint32_t parentIndex) -> uint32_t
    {
        if (!n)
            return U32_MAX;

        const uint32_t thisIndex = static_cast<uint32_t>(nodeRecords.size());
        nodeRecords.push_back(sm::SModelNodeRecord{}); // reserve slot; filled at end

        if (n->mName.length > 0)
            nodeNameToIndex[n->mName.C_Str()] = thisIndex;

        sm::SModelNodeRecord rec{};
        rec.nameStrOffset = strings.add(n->mName.length > 0 ? std::string(n->mName.C_Str()) : std::string());
        rec.parentIndex = (parentIndex == U32_MAX) ? U32_MAX : parentIndex;

        rec.firstPrimitiveIndex = static_cast<uint32_t>(nodePrimitiveIndices.size());
        rec.primitiveCount = 0;
        AiMatToColumnMajor(n->mTransformation, rec.localMatrix);

        // Append primitive indices for meshes under this node
        for (unsigned mi = 0; mi < n->mNumMeshes; ++mi)
        {
            const unsigned aiMeshIdx = n->mMeshes[mi];
            int32_t primIdx = (aiMeshIdx < meshIndexToPrimIndex.size()) ? meshIndexToPrimIndex[aiMeshIdx] : -1;
            if (primIdx >= 0)
            {
                nodePrimitiveIndices.push_back(static_cast<uint32_t>(primIdx));
                rec.primitiveCount++;
            }
        }

        // NEW: explicit direct-children list (works with DFS ordering)
        if (n->mNumChildren > 0)
        {
            rec.firstChildIndex = static_cast<uint32_t>(nodeChildIndices.size());
            rec.childCount = static_cast<uint32_t>(n->mNumChildren);

            // Reserve a contiguous slice for this node's *direct* children.
            // This prevents descendant emissions from interleaving into this node's slice.
            const uint32_t start = rec.firstChildIndex;
            nodeChildIndices.resize(size_t(nodeChildIndices.size()) + size_t(rec.childCount), U32_MAX);

            for (uint32_t ci = 0; ci < rec.childCount; ++ci)
            {
                const uint32_t childIndex = EmitNode(n->mChildren[ci], thisIndex);
                nodeChildIndices[start + ci] = childIndex;
            }
        }
        else
        {
            rec.firstChildIndex = U32_MAX;
            rec.childCount = 0;
        }

        // IMPORTANT: write the record after recursion to avoid invalidated references.
        nodeRecords[thisIndex] = rec;

        return thisIndex;
    };

    (void)EmitNode(scene->mRootNode, U32_MAX);

    // ------------------------------------------------------------
    // Finalize skin tables (resolve joint node names -> node indices)
    // ------------------------------------------------------------
    skinRecords.clear();
    skinJointNodeIndices.clear();
    skinInverseBindMatrices.clear();

    skinRecords.reserve(tmpSkins.size());
    for (const TmpSkin &skin : tmpSkins)
    {
        sm::SModelSkinRecord rec{};
        rec.nameStrOffset = strings.add(skin.name);
        rec.jointCount = static_cast<uint32_t>(skin.jointNodeNames.size());
        rec.firstJointNodeIndex = static_cast<uint32_t>(skinJointNodeIndices.size());
        rec.firstInverseBindMatrix = static_cast<uint32_t>(skinInverseBindMatrices.size());

        for (const std::string &jointName : skin.jointNodeNames)
        {
            auto it = nodeNameToIndex.find(jointName);
            if (it == nodeNameToIndex.end())
            {
                std::cout << "Skin joint node not found in node table: '" << jointName << "'\n";
                return 2;
            }
            skinJointNodeIndices.push_back(it->second);
        }

        skinInverseBindMatrices.insert(skinInverseBindMatrices.end(), skin.inverseBindMatrices.begin(), skin.inverseBindMatrices.end());
        skinRecords.push_back(rec);
    }

    // ------------------------------------------------------------
    // Build animation tables (node TRS only)
    // ------------------------------------------------------------
    if (scene->mNumAnimations > 0)
    {
        for (uint32_t a = 0; a < scene->mNumAnimations; a++)
        {
            const aiAnimation *anim = scene->mAnimations[a];
            if (!anim)
                continue;

            const double tps = (anim->mTicksPerSecond != 0.0) ? anim->mTicksPerSecond : 1.0;

            sm::SModelAnimationClipRecord clip{};

            std::string clipName;
            if (anim->mName.length > 0)
                clipName = anim->mName.C_Str();
            else
                clipName = std::string("Anim_") + std::to_string(a);

            clip.nameOffset = strings.add(clipName);
            clip.durationSec = (float)(anim->mDuration / tps);
            clip.firstChannel = (uint32_t)animChannels.size();
            clip.channelCount = 0;

            // For each node channel
            for (uint32_t c = 0; c < anim->mNumChannels; c++)
            {
                const aiNodeAnim *ch = anim->mChannels[c];
                if (!ch)
                    continue;

                auto it = nodeNameToIndex.find(ch->mNodeName.C_Str());
                if (it == nodeNameToIndex.end())
                {
                    // Node not found: exporters sometimes include channels for nodes not present
                    continue;
                }

                const uint32_t targetNode = it->second;
                const auto interp = sm::SModelAnimInterpolation::Linear;

                // Translation
                if (ch->mNumPositionKeys > 0)
                {
                    const uint16_t samplerIndex = AddVec3Sampler(ch->mPositionKeys,
                                                                 (uint32_t)ch->mNumPositionKeys,
                                                                 tps,
                                                                 interp,
                                                                 animTimes,
                                                                 animValues,
                                                                 animSamplers);

                    sm::SModelAnimationChannelRecord outCh{};
                    outCh.targetNode = targetNode;
                    outCh.path = (uint16_t)sm::SModelAnimPath::Translation;
                    outCh.samplerIndex = samplerIndex;
                    animChannels.push_back(outCh);
                    clip.channelCount++;
                }

                // Rotation
                if (ch->mNumRotationKeys > 0)
                {
                    const uint16_t samplerIndex = AddQuatSampler(ch->mRotationKeys,
                                                                 (uint32_t)ch->mNumRotationKeys,
                                                                 tps,
                                                                 interp,
                                                                 animTimes,
                                                                 animValues,
                                                                 animSamplers);

                    sm::SModelAnimationChannelRecord outCh{};
                    outCh.targetNode = targetNode;
                    outCh.path = (uint16_t)sm::SModelAnimPath::Rotation;
                    outCh.samplerIndex = samplerIndex;
                    animChannels.push_back(outCh);
                    clip.channelCount++;
                }

                // Scale
                if (ch->mNumScalingKeys > 0)
                {
                    const uint16_t samplerIndex = AddVec3Sampler(ch->mScalingKeys,
                                                                 (uint32_t)ch->mNumScalingKeys,
                                                                 tps,
                                                                 interp,
                                                                 animTimes,
                                                                 animValues,
                                                                 animSamplers);

                    sm::SModelAnimationChannelRecord outCh{};
                    outCh.targetNode = targetNode;
                    outCh.path = (uint16_t)sm::SModelAnimPath::Scale;
                    outCh.samplerIndex = samplerIndex;
                    animChannels.push_back(outCh);
                    clip.channelCount++;
                }
            }

            // Store only non-empty clips
            if (clip.channelCount > 0)
            {
                animClips.push_back(clip);
            }
            else
            {
                // Rewind firstChannel to keep clip table consistent (no-op since clip not stored)
                // Channels weren't added either, so nothing to fix.
            }
        }
    }

    // ------------------------------------------------------------
    // Optional clip splitting via sidecar file
    // ------------------------------------------------------------
    // Many DCC exports (e.g. Blender without "All Actions") end up with one big animation ("Take 001").
    // If you want multiple runtime clips (idle/run/attack...), create a sidecar next to the input:
    //   scene.clips.json
    // with contents like:
    // {
    //   "sourceAnim": 0,
    //   "clips": [
    //     {"name":"Idle", "startSec":0.0, "endSec":2.0},
    //     {"name":"Run",  "startSec":2.0, "endSec":3.0}
    //   ]
    // }
    {
        const std::string sidecarA = ReplaceExtension(inputPath, ".clips.json");
        const std::string sidecarB = inputPath + ".clips.json";

        std::string sidecarText;
        std::string sidecarPath;
        if (ReadTextFile(sidecarA, sidecarText))
            sidecarPath = sidecarA;
        else if (ReadTextFile(sidecarB, sidecarText))
            sidecarPath = sidecarB;

        struct SplitDef
        {
            std::string name;
            float startSec = 0.0f;
            float endSec = 0.0f;
        };

        if (!sidecarPath.empty())
        {
            uint32_t sourceAnim = 0;
            {
                std::regex re_src(R"re("sourceAnim"\s*:\s*(\d+))re");
                std::smatch m;
                if (std::regex_search(sidecarText, m, re_src))
                    sourceAnim = static_cast<uint32_t>(std::stoul(m[1].str()));
            }

            std::vector<SplitDef> splits;
            {
                // Extremely small, permissive parser: looks for {"name":"..","startSec":..,"endSec":..}
                std::regex re_clip(R"re(\{[^\}]*?"name"\s*:\s*"([^"]+)"[^\}]*?"startSec"\s*:\s*([-+]?\d*\.?\d+)[^\}]*?"endSec"\s*:\s*([-+]?\d*\.?\d+)[^\}]*?\})re");
                for (auto it = std::sregex_iterator(sidecarText.begin(), sidecarText.end(), re_clip); it != std::sregex_iterator(); ++it)
                {
                    SplitDef d;
                    d.name = (*it)[1].str();
                    d.startSec = std::stof((*it)[2].str());
                    d.endSec = std::stof((*it)[3].str());
                    if (d.endSec > d.startSec)
                        splits.push_back(std::move(d));
                }
            }

            if (!splits.empty() && sourceAnim < animClips.size())
            {
                std::cout << "Using clip split sidecar: " << sidecarPath << " (sourceAnim=" << sourceAnim << ", clips=" << splits.size() << ")\n";

                const auto &srcClip = animClips[sourceAnim];
                const uint32_t srcFirst = srcClip.firstChannel;
                const uint32_t srcCount = srcClip.channelCount;

                std::vector<sm::SModelAnimationClipRecord> newClips;
                std::vector<sm::SModelAnimationChannelRecord> newChannels;
                std::vector<sm::SModelAnimationSamplerRecord> newSamplers;
                std::vector<float> newTimes;
                std::vector<float> newValues;

                newClips.reserve(splits.size());
                newChannels.reserve(size_t(srcCount) * splits.size());
                newSamplers.reserve(size_t(srcCount) * splits.size());

                for (const SplitDef &sd : splits)
                {
                    sm::SModelAnimationClipRecord outClip{};
                    outClip.nameOffset = strings.add(sd.name);
                    outClip.durationSec = sd.endSec - sd.startSec;
                    outClip.firstChannel = static_cast<uint32_t>(newChannels.size());
                    outClip.channelCount = 0;

                    for (uint32_t ci = 0; ci < srcCount; ++ci)
                    {
                        const uint32_t chIx = srcFirst + ci;
                        if (chIx >= animChannels.size())
                            break;
                        const auto &srcCh = animChannels[chIx];
                        if (srcCh.samplerIndex >= animSamplers.size())
                            continue;
                        const auto &srcS = animSamplers[srcCh.samplerIndex];

                        if (srcS.timeCount == 0)
                            continue;
                        if (srcS.firstTime + srcS.timeCount > animTimes.size())
                            continue;

                        const uint32_t stride = (srcS.valueType == (uint8_t)sm::SModelAnimValueType::Vec3) ? 3u : 4u;
                        if (srcS.firstValue + srcS.valueCount > animValues.size())
                            continue;
                        if (srcS.valueCount < stride)
                            continue;

                        const float *times = animTimes.data() + srcS.firstTime;
                        const float *values = animValues.data() + srcS.firstValue;
                        const uint32_t keyCount = srcS.timeCount;

                        // Build clipped keys (always includes boundaries).
                        std::vector<float> clipTimes;
                        std::vector<float> clipValues;
                        clipTimes.reserve(keyCount + 2);
                        clipValues.reserve((keyCount + 2) * stride);

                        const float tStart = sd.startSec;
                        const float tEnd = sd.endSec;
                        if (!(tEnd > tStart))
                            continue;

                        // Start sample
                        clipTimes.push_back(0.0f);
                        if (stride == 3)
                        {
                            float v[3];
                            SampleVec3At(times, values, keyCount, tStart, v);
                            clipValues.insert(clipValues.end(), v, v + 3);
                        }
                        else
                        {
                            float q[4];
                            SampleQuatAt(times, values, keyCount, tStart, q);
                            clipValues.insert(clipValues.end(), q, q + 4);
                        }

                        // Interior exact keys
                        for (uint32_t k = 0; k < keyCount; ++k)
                        {
                            const float tk = times[k];
                            if (!(tk > tStart && tk < tEnd))
                                continue;
                            clipTimes.push_back(tk - tStart);
                            const float *vk = values + size_t(k) * stride;
                            clipValues.insert(clipValues.end(), vk, vk + stride);
                        }

                        // End sample
                        clipTimes.push_back(tEnd - tStart);
                        if (stride == 3)
                        {
                            float v[3];
                            SampleVec3At(times, values, keyCount, tEnd, v);
                            clipValues.insert(clipValues.end(), v, v + 3);
                        }
                        else
                        {
                            float q[4];
                            SampleQuatAt(times, values, keyCount, tEnd, q);
                            clipValues.insert(clipValues.end(), q, q + 4);
                        }

                        // Create new sampler
                        sm::SModelAnimationSamplerRecord outS{};
                        outS.firstTime = static_cast<uint32_t>(newTimes.size());
                        outS.timeCount = static_cast<uint32_t>(clipTimes.size());
                        outS.firstValue = static_cast<uint32_t>(newValues.size());
                        outS.valueCount = static_cast<uint32_t>(clipValues.size());
                        outS.interpolation = (uint8_t)sm::SModelAnimInterpolation::Linear;
                        outS.valueType = srcS.valueType;

                        newTimes.insert(newTimes.end(), clipTimes.begin(), clipTimes.end());
                        newValues.insert(newValues.end(), clipValues.begin(), clipValues.end());

                        const uint32_t outSamplerIndex = static_cast<uint32_t>(newSamplers.size());
                        newSamplers.push_back(outS);

                        sm::SModelAnimationChannelRecord outCh{};
                        outCh.targetNode = srcCh.targetNode;
                        outCh.path = srcCh.path;
                        outCh.samplerIndex = static_cast<uint16_t>(outSamplerIndex);
                        newChannels.push_back(outCh);
                        outClip.channelCount++;
                    }

                    if (outClip.channelCount > 0)
                        newClips.push_back(outClip);
                }

                if (!newClips.empty())
                {
                    animClips = std::move(newClips);
                    animChannels = std::move(newChannels);
                    animSamplers = std::move(newSamplers);
                    animTimes = std::move(newTimes);
                    animValues = std::move(newValues);
                }
            }
        }
    }

    // ------------------------------------------------------------
    // Print animation clip summary (helps map indices into JSON)
    // ------------------------------------------------------------
    if (!animClips.empty())
    {
        std::cout << "\n=== SMODEL Animation Clips (index -> name/tag) ===\n";
        for (uint32_t i = 0; i < static_cast<uint32_t>(animClips.size()); ++i)
        {
            const auto &c = animClips[i];
            const char *nm = strings.data.empty() ? "" : (strings.data.data() + c.nameOffset);
            const std::string name = (nm && nm[0] != '\0') ? std::string(nm) : (std::string("clip_") + std::to_string(i));
            std::cout << "[clip " << i << "] "
                      << "tag=" << InferAnimTag(name) << " "
                      << "duration=" << c.durationSec << "s "
                      << "channels=" << c.channelCount << " "
                      << "name=\"" << name << "\"\n";
        }
        std::cout << "===============================================\n\n";

        if (animClips.size() == 1)
        {
            std::cout << "Note: This asset has only 1 animation clip. If you expected multiple (idle/run/etc), export multiple glTF animations (e.g., Blender: enable 'All Actions') or provide a scene.clips.json sidecar to split the timeline.\n\n";
        }
    }

    // Build header offsets
    // File layout:
    // Header
    // MeshRecords
    // PrimitiveRecords
    // MaterialRecords
    // TextureRecords
    // NodeRecords
    // NodePrimitiveIndices
    // NodeChildIndices
    // SkinRecords
    // SkinJointNodeIndices
    // SkinInverseBindMatrices
    // Anim*
    // StringTable
    // Blob
    // ------------------------------------------------------------
    sm::SModelHeader header{};
    header.magic = sm::SMODEL_MAGIC;
    header.versionMajor = 4;
    header.versionMinor = 0;

    header.meshCount = static_cast<uint32_t>(meshRecords.size());
    header.primitiveCount = static_cast<uint32_t>(primRecords.size());
    header.materialCount = static_cast<uint32_t>(materialRecords.size());
    header.textureCount = static_cast<uint32_t>(textureRecords.size());
    header.nodeCount = static_cast<uint32_t>(nodeRecords.size());
    header.nodePrimitiveIndexCount = static_cast<uint32_t>(nodePrimitiveIndices.size());
    header.nodeChildIndicesCount = static_cast<uint32_t>(nodeChildIndices.size());

    header.skinCount = static_cast<uint32_t>(skinRecords.size());
    header.skinJointNodeIndicesCount = static_cast<uint32_t>(skinJointNodeIndices.size());
    header.skinInverseBindMatricesCount = static_cast<uint32_t>(skinInverseBindMatrices.size());
    header.animClipsCount = static_cast<uint32_t>(animClips.size());
    header.animChannelsCount = static_cast<uint32_t>(animChannels.size());
    header.animSamplersCount = static_cast<uint32_t>(animSamplers.size());
    header.animTimesCount = static_cast<uint32_t>(animTimes.size());
    header.animValuesCount = static_cast<uint32_t>(animValues.size());

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

    // Node child indices
    header.nodeChildIndicesOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(nodeChildIndices.size()) * sizeof(uint32_t);

    // Skins (V4)
    header.skinsOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(skinRecords.size()) * sizeof(sm::SModelSkinRecord);

    header.skinJointNodeIndicesOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(skinJointNodeIndices.size()) * sizeof(uint32_t);

    header.skinInverseBindMatricesOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(skinInverseBindMatrices.size()) * sizeof(float);

    // Animations
    header.animClipsOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(animClips.size()) * sizeof(sm::SModelAnimationClipRecord);

    header.animChannelsOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(animChannels.size()) * sizeof(sm::SModelAnimationChannelRecord);

    header.animSamplersOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(animSamplers.size()) * sizeof(sm::SModelAnimationSamplerRecord);

    header.animTimesOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(animTimes.size()) * sizeof(float);

    header.animValuesOffset = static_cast<uint32_t>(cursor);
    cursor += uint64_t(animValues.size()) * sizeof(float);

    header.stringTableOffset = cursor;
    header.stringTableSize = static_cast<uint64_t>(strings.data.size());
    cursor += strings.data.size();

    header.blobOffset = cursor;
    header.blobSize = static_cast<uint64_t>(blob.bytes.size());
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
    WriteVector(out, nodeChildIndices);
    WriteVector(out, skinRecords);
    WriteVector(out, skinJointNodeIndices);
    WriteVector(out, skinInverseBindMatrices);
    WriteVector(out, animClips);
    WriteVector(out, animChannels);
    WriteVector(out, animSamplers);
    WriteVector(out, animTimes);
    WriteVector(out, animValues);
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
    std::cout << "Skins      : " << header.skinCount << "\n";
    std::cout << "AnimClips  : " << header.animClipsCount << "\n";
    std::cout << "AnimChans  : " << header.animChannelsCount << "\n";
    std::cout << "AnimSamplers: " << header.animSamplersCount << "\n";
    std::cout << "AnimTimes  : " << header.animTimesCount << " floats\n";
    std::cout << "AnimValues : " << header.animValuesCount << " floats\n";
    std::cout << "StringTable: " << header.stringTableSize << " bytes\n";
    std::cout << "Blob       : " << header.blobSize << " bytes\n";
    std::cout << "FileSize   : " << header.fileSizeBytes << " bytes\n";

    return 0;
}
