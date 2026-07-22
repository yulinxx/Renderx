#version 460 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;

uniform mat4 uModelMatrix;
uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;

out vec3 vNormal;

void main()
{
    vec4 worldPos = uModelMatrix * vec4(aPosition, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
    vNormal = mat3(uModelMatrix) * aNormal;
}