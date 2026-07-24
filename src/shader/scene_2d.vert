#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;

uniform mat3 uViewMatrix;

out vec3 vColor;

void main()
{
    vec3 pos = uViewMatrix * vec3(aPosition.xy, 1.0);
    gl_Position = vec4(pos.xy, aPosition.z, 1.0);
    vColor = aColor;
}
