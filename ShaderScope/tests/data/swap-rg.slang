#version 450

// Shared fixture pass: swaps the red and green channels, so red input
// becomes green output. Lets tests distinguish a correctly-bound semantic
// texture (green) from the degrade-to-original fallback (red).

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
layout(set = 0, binding = 2) uniform sampler2D Source;

void main()
{
   vec3 c = texture(Source, vTexCoord).rgb;
   FragColor = vec4(c.g, c.r, c.b, 1.0);
}
