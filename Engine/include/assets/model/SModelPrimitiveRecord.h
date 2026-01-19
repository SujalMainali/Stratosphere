#pragma once
#include <cstdint>

namespace Engine::smodel
{
#pragma pack(push, 1)

    // ============================================================
    // Primitive Record (V1)
    // ============================================================
    // A primitive is the smallest renderable unit:
    // It references a mesh + a material (like glTF primitives).
    //
    // For now, most primitives will simply draw the entire mesh.
    // But firstIndex/indexCount allow future submesh slicing without format change.
    struct SModelPrimitiveRecord
    {
        uint32_t meshIndex;     // index into mesh records
        uint32_t materialIndex; // index into material records

        uint32_t firstIndex; // start index (0 in most cases)
        uint32_t indexCount; // how many indices to draw

        int32_t vertexOffset; // usually 0 (useful if merged meshes later)
        uint32_t reserved;
    };

#pragma pack(pop)

    static_assert(sizeof(SModelPrimitiveRecord) == 24, "SModelPrimitiveRecord size mismatch");

} // namespace Engine::smodel