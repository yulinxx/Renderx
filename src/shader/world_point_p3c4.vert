// 世界空间点图元 / P3C4（带透明度）
//
// 见 world_point_p3c3.vert 的说明：点尺寸走 gl_PointSize 而非顶点几何。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

out vec4 vColor;

void main()
{
    vColor = aColor;
    gl_Position = uView * vec4(aPos, 1.0);
    gl_PointSize = uPointSize;
}
