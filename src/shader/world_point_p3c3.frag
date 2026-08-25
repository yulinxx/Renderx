// 点图元片段着色器 —— 把方形 point sprite 裁成圆形
//
// gl_PointCoord 在 [0,1]^2，映射到 [-1,1]^2 后按单位圆丢弃外部片段。
#version 330 core

in vec3 vColor;

out vec4 frag;

void main()
{
    vec2 d = gl_PointCoord * 2.0 - 1.0;
    if (dot(d, d) > 1.0)
        discard;
    frag = vec4(vColor, 1.0);
}
