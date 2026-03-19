#version 450

// SModel v4 vertex layout (VertexPNTTJW):
// location 0: vec3 position
// location 1: vec3 normal
// location 2: vec2 uv0
// location 3: vec4 tangent
// location 8: uvec4 joints (u16x4)
// location 9: vec4 weights
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV0;
layout(location = 3) in vec4 inTangent;

layout(location = 8) in uvec4 inJoints;
layout(location = 9) in vec4 inWeights;

layout(set = 0, binding = 0) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} cam;

// Flattened node globals: [instance][node]
layout(set = 0, binding = 1, std430) readonly buffer NodePalette
{
    mat4 nodeGlobals[];
} palette;

// Flattened joint matrices: [instance][joint]
layout(set = 0, binding = 2, std430) readonly buffer JointPalette
{
    mat4 jointMats[];
} joints;

// Slot-indexed instance world transforms: [slot]
layout(set = 0, binding = 3, std430) readonly buffer InstanceWorlds
{
    mat4 instanceWorlds[];
} inst;

// Active slot indirection: gl_InstanceIndex -> slot
layout(set = 0, binding = 4, std430) readonly buffer ActiveSlots
{
    uint slotIndex[];
} activeSlots;

layout(push_constant) uniform PushConstants
{
    mat4 model;
    vec4 baseColorFactor;
    vec4 materialParams;
    uvec4 nodeInfo; // x=nodeIndex, y=nodeCount
    uvec4 skinInfo; // x=skinBaseJoint, y=skinJointCount, z=jointPaletteStride
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV0;

void main()
{
    uint instanceIndex = uint(gl_InstanceIndex);
    uint slot = activeSlots.slotIndex[instanceIndex];
    mat4 instanceWorld = inst.instanceWorlds[slot];
    uint nodeIndex = pc.nodeInfo.x;
    uint nodeCount = max(pc.nodeInfo.y, 1u);

    uint skinBase = pc.skinInfo.x;
    uint skinJointCount = pc.skinInfo.y;
    uint jointStride = max(pc.skinInfo.z, 1u);

    mat4 M;
    vec4 modelPos;
    vec3 modelNormal;

    if (skinJointCount > 0u)
    {
        // Skinning: joint matrices already bring vertices into model space.
        // Build a weighted skin matrix from up to 4 joints.
        mat4 skinM = mat4(0.0);
        vec4 w = inWeights;

        // Clamp joint indices to skinJointCount to avoid OOB.
        uvec4 j = min(inJoints, uvec4(max(skinJointCount - 1u, 0u)));

        uint base = slot * jointStride + skinBase;
        skinM += w.x * joints.jointMats[base + j.x];
        skinM += w.y * joints.jointMats[base + j.y];
        skinM += w.z * joints.jointMats[base + j.z];
        skinM += w.w * joints.jointMats[base + j.w];

        modelPos = skinM * vec4(inPosition, 1.0);
        modelNormal = normalize(mat3(skinM) * inNormal);

        M = instanceWorld * pc.model;
    }
    else
    {
        // Unskinned: use node transform palette.
        mat4 nodeM = palette.nodeGlobals[slot * nodeCount + nodeIndex];
        M = instanceWorld * pc.model * nodeM;
        modelPos = vec4(inPosition, 1.0);
        modelNormal = inNormal;
    }

    vec4 worldPos = M * modelPos;
    mat3 normalMat = mat3(transpose(inverse(M)));
    vNormal = normalize(normalMat * modelNormal);
    vUV0 = inUV0;
    gl_Position = cam.proj * cam.view * worldPos;
}
