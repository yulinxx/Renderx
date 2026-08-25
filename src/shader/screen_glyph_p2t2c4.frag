// 字形四边形 / P2T2C4 —— 图集是 R8 覆盖率，颜色来自顶点
#version 330 core

in vec2 vUV;
in vec4 vColor;

// 字形图集（Format::R8Unorm）。GL 在采样单通道纹理时 g/b 为 0、a 为 1，
// 因此**不能**像位图那样直接 texture() * vColor —— 那会把字画成红色。
uniform sampler2D uTex;

out vec4 frag;

void main()
{
    float coverage = texture(uTex, vUV).r;
    // rgb 全取顶点色、alpha 乘覆盖率：同一份图集可以画任意颜色的文字。
    // 覆盖率是 stb_truetype 光栅化出的抗锯齿灰度，直接当 alpha 用即可；
    // 旧的 text_sdf.frag 对它做 smoothstep 把它当距离场，等于硬阈值化，
    // 反而把这条边缘信息削掉了。
    frag = vec4(vColor.rgb, vColor.a * coverage);
}
