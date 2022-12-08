#version 450

layout (location = 0) in vec3 inPos;
layout (location = 1) in vec3 inColor;

layout (binding = 0) uniform UBO 
{
	mat4 projectionMatrix;
	mat4 modelMatrix;
	mat4 viewMatrix;
} ubo;

layout (location = 0) out vec3 outColor;

out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
	// i imagine this is maybe the worst way i could have possibly done the color thing
	// but i couldn't even figure out *where* in the game the previous color was hard-coded
	// so i'm hard-coding my own color further down the line
	// i also tried to make a separate shader that would make the traffic red
	// but i couldn't find where the shaders are turned into spv files either
	// so i guess the traffic is green too
	// oh well
	outColor = vec3(0, 1, 0);
	gl_Position = ubo.projectionMatrix * ubo.viewMatrix * ubo.modelMatrix * vec4(inPos.xyz, 1.0);
}
