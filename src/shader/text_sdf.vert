#version 460 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform mat3 uViewMatrix;
uniform vec2 uViewportSize;

out vec2 vTexCoord;
out vec4 vColor;

void main()
{
    vec3 pos = uViewMatrix * vec3(aPosition, 1.0);
    gl_Position = vec4(pos.xy, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}