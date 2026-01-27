#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    enum class SModelAnimPath : uint16_t
    {
        Translation = 0,
        Rotation = 1,
        Scale = 2,
    };

    enum class SModelAnimInterpolation : uint8_t
    {
        Step = 0,
        Linear = 1,
        CubicSpline = 2,
    };

    enum class SModelAnimValueType : uint8_t
    {
        Vec3 = 0,
        Quat = 1,
    };

    struct SModelAnimationClipRecord
    {
        uint32_t nameOffset; // string table offset (0 if none)
        float durationSec;   // clip duration in seconds
        uint32_t firstChannel;
        uint32_t channelCount;
    };

    struct SModelAnimationChannelRecord
    {
        uint32_t targetNode;   // node index
        uint16_t path;         // SModelAnimPath
        uint16_t samplerIndex; // index into AnimationSamplerRecord table
    };

    struct SModelAnimationSamplerRecord
    {
        uint32_t firstTime; // index into animTimes (float)
        uint32_t timeCount;

        uint32_t firstValue; // index into animValues (float)
        uint32_t valueCount; // floats count (timeCount*3 or timeCount*4)

        uint8_t interpolation; // SModelAnimInterpolation
        uint8_t valueType;     // SModelAnimValueType
        uint16_t _pad;
    };

#pragma pack(pop)

    static_assert(sizeof(SModelAnimationClipRecord) == 16, "SModelAnimationClipRecord size mismatch");
    static_assert(sizeof(SModelAnimationChannelRecord) == 8, "SModelAnimationChannelRecord size mismatch");
    static_assert(sizeof(SModelAnimationSamplerRecord) == 20, "SModelAnimationSamplerRecord size mismatch");

} // namespace Engine::smodel
