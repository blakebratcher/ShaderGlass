#version 450
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uSource;
layout(push_constant) uniform PC { vec4 uvTransform; } pc;
void main() {
    // uvTransform = (u0, v0, u1, v1): mix maps [0,1] UV to the crop window.
    // Default (0,0,1,1) is identity — full source texture.
    vec2 sampleUV = mix(pc.uvTransform.xy, pc.uvTransform.zw, vUV);
    outColor = texture(uSource, sampleUV);
}
