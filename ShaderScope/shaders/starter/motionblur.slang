#version 450

// Motion blur — averages the current frame with the previous two original
// frames (OriginalHistory1/2). Moving content leaves a short trail; static
// content is unchanged. Demonstrates the frame-history ring buffer.

#pragma parameter BLUR_STRENGTH "Blur Strength" 0.5 0.0 1.0 0.05

layout(std140, set = 0, binding = 0) uniform UBO
{
	mat4 MVP;
	float BLUR_STRENGTH;
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
layout(set = 0, binding = 3) uniform sampler2D OriginalHistory1;
layout(set = 0, binding = 4) uniform sampler2D OriginalHistory2;

void main()
{
   vec3 now  = texture(Source, vTexCoord).rgb;
   vec3 h1   = texture(OriginalHistory1, vTexCoord).rgb;
   vec3 h2   = texture(OriginalHistory2, vTexCoord).rgb;
   vec3 blur = (now + h1 + h2) / 3.0;
   FragColor = vec4(mix(now, blur, global.BLUR_STRENGTH), 1.0);
}
