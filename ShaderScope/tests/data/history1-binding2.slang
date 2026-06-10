#version 450

// Regression fixture: samples ONLY OriginalHistory1 — no Source sampler —
// with the history sampler sitting at binding 2, which is exactly the
// historic fallback slot the runtime uses for an un-reflected Source.
// The semantic view must win over the phantom Source write there.

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
layout(set = 0, binding = 2) uniform sampler2D OriginalHistory1;

void main()
{
   FragColor = vec4(texture(OriginalHistory1, vTexCoord).rgb, 1.0);
}
