// 屏幕空间带纹理 / P2T2C4（2D 位置 + UV + RGBA）
//
// 用于文本字形四边形与位图。颜色作为纹理的乘性调制。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

out vec2 vUV;
out vec4 vColor;

void main()
{
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    vUV = aUV;
    vColor = aColor;
    gl_Position = vec4(ndc, 0.0, 1.0);
}
