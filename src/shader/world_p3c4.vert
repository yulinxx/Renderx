// 世界空间 / P3C4（位置 + RGBA）
//
// 覆盖层（选择框、手柄、虚线轮廓、点标记、捕捉圈）统一走世界空间 + P3C4：
// 世界空间保证缩放时覆盖层与图元几何一致变换，alpha 通道支持半透明装饰。
// 该决策见 Docs/03-渲染主链/新渲染架构.md §13.2。
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

uniform mat4 uView;

out vec4 vColor;

void main()
{
    vColor = aColor;
    gl_Position = uView * vec4(aPos, 1.0);
}
