#version 450

// Final fixture pass: outputs PassOutput0 — pass 0's output from THIS
// frame — ignoring its own Source (pass 1's inverted image).

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
layout(set = 0, binding = 3) uniform sampler2D PassOutput0;

void main()
{
   FragColor = vec4(texture(PassOutput0, vTexCoord).rgb, 1.0);
}
