#version 450

layout(location = 0) in vec2 inPos;
layout(location = 1) in vec3 inColor;

layout(push_constant) uniform Push {
    vec2 offset;
} pc;

layout(location = 0) out vec3 fragColor;

void main() {
    vec2 pos = inPos + pc.offset;
    gl_Position = vec4(pos, 0.0, 1.0);
    fragColor = inColor;
}