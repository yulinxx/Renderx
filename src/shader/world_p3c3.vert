// 世界空间 / P3C3（位置 + RGB 颜色）
//
// 顶点经 uView 变换。P3C3 是不含透明度的基础格式，用于图元几何本身。
// 注意：属性槽位必须显式写 layout(location=N)——Apple 的 GLSL 编译器
// 在缺省时会给出与声明顺序不一致的槽位（见 Docs/Mac渲染.md §9）。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

out vec3 vColor;

void main()
{
    vColor = aColor;
    gl_Position = uView * vec4(aPos, 1.0);
}
