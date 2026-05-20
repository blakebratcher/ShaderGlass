#version 450
layout(push_constant) uniform Push { vec4 _unused; } params;

#pragma stage vertex
layout(location = 0) out vec2 vTexCoord;
void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vTexCoord = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 0) uniform sampler2D Source;
void main() {
    FragColor = texture(Source, vTexCoord);
}
