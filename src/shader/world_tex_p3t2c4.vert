// 世界空间带纹理 / P3T2C4（3D 位置 + UV + RGBA）
//
// 位图实体走这条：顶点坐标是**世界坐标**，因此贴图随平移/缩放一起变换。
// 旋转与任意四点变换在 CPU 侧算进顶点，DLL 不需要额外的变换矩阵接口。
// 颜色作为纹理的乘性调制（整体透明度、着色）。
//
// 与 screen_tex_p2t2c4.vert 的唯一区别就是这里乘 uView、那里把位置当像素坐标。
// 片元复用 screen_tex_p2t2c4.frag：它只用 vUV / vColor，与空间无关。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

out vec2 vUV;
out vec4 vColor;

void main()
{
    vUV = aUV;
    vColor = aColor;
    gl_Position = uView * vec4(aPos, 1.0);
}
