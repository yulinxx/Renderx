// 屏幕空间点图元 / P3C3
//
// 与 screen_p3c3.vert 的唯一差别是写 gl_PointSize。
// 需要 GL_PROGRAM_POINT_SIZE 已启用。
#version 330 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform vec2 uViewport;
uniform float uPointSize;

out vec3 vColor;

void main()
{
    vec2 ndc = vec2(aPos.x / uViewport.x * 2.0 - 1.0,
                    1.0 - aPos.y / uViewport.y * 2.0);
    vColor = aColor;
    gl_Position = vec4(ndc, 0.0, 1.0);
    gl_PointSize = uPointSize;
}
