// 屏幕空间 / P3C4
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

out vec4 vColor;

void main()
{
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
