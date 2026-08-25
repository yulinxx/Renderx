// 世界空间点图元 / P3C3
//
// 与 world_p3c3.vert 的唯一差别是写 gl_PointSize：点标记的尺寸是**像素**，
// 不应随视图缩放变化，因此尺寸不能由顶点几何表达，只能走 gl_PointSize。
// 需要 GL_PROGRAM_POINT_SIZE 已启用（GL 后端在设备创建时统一开启）。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

out vec3 vColor;

void main()
{
    vColor = aColor;
    gl_Position = uView * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
