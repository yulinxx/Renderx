// 字形四边形 / P3T2C4 —— 图集是 R8 **有符号距离场**，颜色来自顶点
#version 330 core

in vec2 vUV;
in vec4 vColor;

// 字形图集（Format::R8Unorm）。内容不是覆盖率而是距离场：
// 值 128/255 ≈ 0.5 表示该点恰在字形轮廓上，越大越深入字形内部。
// 由 stbtt_GetGlyphSDF 生成，见 FontDesc::sdfPadding。
uniform sampler2D uTex;

out vec4 frag;

void main()
{
    // 归一化的符号距离：>0 在字形内，<0 在字形外
    float d = texture(uTex, vUV).r - 0.5;

    // 这一行是「一张图集服务所有缩放」的成立条件。
    //
    // fwidth 求的是相邻屏幕像素间 d 的变化量，也就是「一个屏幕像素跨越多少
    // 距离场单位」。用它当 smoothstep 的窗口，抗锯齿过渡宽度就恒等于约一个
    // 屏幕像素，与当前缩放、与图集的光栅化精度都无关 —— 放大到十倍不糊，
    // 缩小到十分之一也不出锯齿。
    //
    // 换成固定窗口（例如 smoothstep(-0.1, 0.1, d)）就退化成旧 text_sdf.frag
    // 的行为：某一个特定缩放下勉强可看，其余缩放要么锯齿要么发虚。
    //
    // 下限保护：极端放大时 fwidth 趋于 0，窗口退化会让 smoothstep 变成硬阈值。
    float w = max(fwidth(d), 1e-5);

    float alpha = smoothstep(-w, w, d);

    // rgb 全取顶点色、alpha 乘覆盖率：同一份图集可以画任意颜色的文字。
    frag = vec4(vColor.rgb, vColor.a * alpha);
}
