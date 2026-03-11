#version 450

layout(set = 1, binding = 0) uniform UBO {
    float angle;
    float padding1;
    float padding2;
    float padding3;
};

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 fragColor;

void main() {
    float c = cos(angle);
    float s = sin(angle);
    vec3 pos = vec3(
        inPos.x * c - inPos.y * s,
        inPos.x * s + inPos.y * c,
        inPos.z
    );
    gl_Position = vec4(pos, 1.0);
    fragColor = inColor;
}
