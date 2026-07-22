#version 460 core

layout(location = 0) in vec3 aPosition;

uniform mat4 uModelMatrix;
uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;

void main()
{
    vec4 worldPos = uModelMatrix * vec4(aPosition, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
}