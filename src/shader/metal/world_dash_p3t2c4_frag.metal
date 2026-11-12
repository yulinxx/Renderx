// 世界空间虚线 / P3T2C4 —— 片段阶段
//
// 与 GLSL 版（src/shader/world_dash_p3t2c4.frag）逐行对应。
//
// 顶点阶段复用 world_tex_p3t2c4_vert.metal（P3T2C4 + 世界空间）。与
// WorldTextured 的差别只在片元：这里不采样贴图，而是把 vUV.x（沿折线的
// 全局弧长）当作相位坐标，用 discard_fragment 表达 dash/gap。这样每次动画帧
// 只更新一次 material 参数，不需要在 CPU 上重新切段。
//
// dash 参数不走上层 uniform（本 DLL 没有通用 uniform 接口），而是借用
// material 的 diffuse 段（与 GLSL 版同一约定）：x=相位（世界弧长）、
// y=实线长、z=空档长、w=半宽（物理像素）。
#include <metal_stdlib>
using namespace metal;

#include "rx_push_constants.metal"

// 与顶点着色器一致的 varying 名字——MSL 的 [[stage_in]] 按**字段名**匹配。
struct RxFragmentIn
{
    float2 vUV;
    float4 vColor;
};

// GLSL 的 mod(x, y) 是 x - y * floor(x / y)，而 MSL 的 fmod 是截断取余：
// 当 x 为负时两者结果不同。宿主把相位归一化到 [0, period)，但轮廓起点处
// vUV.x - phase 仍可能为负，用 fmod 会得到负的相对弧长，进而让
// distDash 变负、整段被错误淡出。这里显式实现地板语义。
static inline float glslMod(float x, float y)
{
    return x - y * floor(x / y);
}

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        constant RxPushConstants& pc [[buffer(30)]])
{
    const float phase = pc.uMatDiffuse.x;
    const float dashLen = pc.uMatDiffuse.y;
    const float gapLen = pc.uMatDiffuse.z;
    const float halfWidthPx = pc.uMatDiffuse.w;

    // 宽度方向羽化：|v| 从 0（中心线）到 1（外缘），换算成偏离边缘的物理像素数。
    // +0.5 的覆盖率近似让 1 像素宽的线在像素中心不透明、在外缘半透明，
    // 因此 DPR=1 与 DPR=2 下的粗细过渡都只跨约一个物理像素。
    const float distPx = abs(in.vUV.y) * halfWidthPx;
    const float alphaEdge = clamp(halfWidthPx - distPx + 0.5f, 0.0f, 1.0f);

    const float period = dashLen + gapLen;
    if (period <= 1e-6f || dashLen >= period)
    {
        // 退化节距或实线：不做 dash 裁剪
        return float4(in.vColor.rgb, in.vColor.a * alphaEdge);
    }

    // 与 CPU 路径同一条公式：mod(s - phase, period) < dash 处为实线。
    // phase 由宿主先归一化到 [0, period)，避免大数取模的精度损失。
    const float rel = glslMod(in.vUV.x - phase, period);
    if (rel >= dashLen)
    {
        discard_fragment();
    }

    // dash 两端各做约 1 物理像素羽化。fwidth 给出「一个屏幕像素跨多少弧长」，
    // 与当前缩放和 DPR 无关，因此不需要额外的屏幕尺寸参数。
    const float aaDash = max(fwidth(in.vUV.x), 1e-6f);
    const float distDash = min(rel, dashLen - rel);
    const float alphaDash = clamp(distDash / aaDash + 0.5f, 0.0f, 1.0f);

    const float alpha = in.vColor.a * alphaEdge * alphaDash;
    if (alpha <= 0.003f)
    {
        discard_fragment();
    }

    return float4(in.vColor.rgb, alpha);
}
