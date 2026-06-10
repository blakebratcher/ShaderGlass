#version 450

// Frame-history fixture: output is OriginalHistory1 (the previous frame's
// original input). Frame 1 must render black (no history yet); frame N
// must render frame N-1's input.

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
} global;

#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;

void main()
{
   gl_Position = global.MVP * Position;
   vTexCoord = TexCoord;
}

#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 3) uniform sampler2D OriginalHistory1;

void main()
{
   FragColor = vec4(texture(OriginalHistory1, vTexCoord).rgb, 1.0);
}
