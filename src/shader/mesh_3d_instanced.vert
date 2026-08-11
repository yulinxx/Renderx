#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in mat4 aModelMatrix;

uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;

out vec3 vNormal;

void main()
{
    vec4 worldPos = aModelMatrix * vec4(aPosition, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
    vNormal = mat3(aModelMatrix) * aNormal;
}