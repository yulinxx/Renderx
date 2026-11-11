// 世界空间虚线 / P3T2C4 —— UV.x 是沿折线的全局弧长，UV.y 是宽度方向坐标（-1..1）
//
// 顶点复用 world_tex_p3t2c4.vert（同为 P3T2C4 + 世界空间）。与 WorldTextured
// 的差别只在片元：这里不采样贴图，而是把弧长当作相位坐标，用 discard 表达
// dash/gap。这样每次动画帧只更新一次 material 参数，不需要在 CPU 上重新切段。
//
// dash 参数不走上层 uniform（本 DLL 没有通用 uniform 接口），而是借用
// material 的 diffuse 段：x=相位（世界弧长）、y=实线长、z=空档长、
// w=半宽（物理像素）。宿主侧见 OverlayScene 的虚线 GPU 分支。
#version 330 core

#include "rx_push_constants.glsl"

in vec2 vUV;
in vec4 vColor;

out vec4 frag;

void main()
{
    float phase = uMatDiffuse.x;
    float dashLen = uMatDiffuse.y;
    float gapLen = uMatDiffuse.z;
    float halfWidthPx = uMatDiffuse.w;

    // 宽度方向羽化：|v| 从 0（中心线）到 1（外缘），换算成偏离边缘的物理像素数。
    // +0.5 的覆盖率近似让 1 像素宽的线在像素中心不透明、在外缘半透明，
    // 因此 DPR=1 与 DPR=2 下的粗细过渡都只跨约一个物理像素。
    float distPx = abs(vUV.y) * halfWidthPx;
    float alphaEdge = clamp(halfWidthPx - distPx + 0.5, 0.0, 1.0);

    float period = dashLen + gapLen;
    if (period <= 1e-6 || dashLen >= period)
    {
        // 退化节距或实线：不做 dash 裁剪
        frag = vec4(vColor.rgb, vColor.a * alphaEdge);
        return;
    }

    // 与 CPU 路径同一条公式：mod(s - phase, period) < dash 处为实线。
    // phase 由宿主先归一化到 [0, period)，避免大数取模的精度损失。
    float rel = mod(vUV.x - phase, period);
    if (rel >= dashLen)
    {
        discard;
    }

    // dash 两端各做约 1 物理像素羽化。fwidth 给出「一个屏幕像素跨多少弧长」，
    // 与当前缩放和 DPR 无关，因此不需要额外的屏幕尺寸参数。
    float aaDash = max(fwidth(vUV.x), 1e-6);
    float distDash = min(rel, dashLen - rel);
    float alphaDash = clamp(distDash / aaDash + 0.5, 0.0, 1.0);

    float alpha = vColor.a * alphaEdge * alphaDash;
    if (alpha <= 0.003)
    {
        discard;
    }

    frag = vec4(vColor.rgb, alpha);
}
