// 点图元片段着色器（P3C4）—— 把方形 point sprite 裁成圆形，保留 alpha
//
// gl_PointCoord 在 [0,1]^2，映射到 [-1,1]^2 后按单位圆丢弃外部片段。
// 与 world_point_p3c3.frag 的差别仅在于 alpha 来自顶点色而非常量 1.0。
#version 330 core

in vec4 vColor;

out vec4 frag;

void main()
{
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    if (dot(d, d) > 1.0)
        discard;
    frag = vColor;
}
