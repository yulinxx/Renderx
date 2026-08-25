// 屏幕空间 / P3C3
//
// aPos.xy 直接是像素坐标（左上原点），在此转 NDC。
// 屏幕空间图元不随视图平移与缩放变化（RenderSpace::Screen），
// 用于标尺、HUD 一类元素。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

out vec3 vColor;

void main()
{
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
