// 字形四边形 / P3T2C4 —— 图集是 R8 有符号距离场，颜色来自顶点（片段阶段）
//
// 与 GLSL 版（src/shader/world_glyph_sdf_p3t2c4.frag）逐行对应。
//
// 顶点阶段复用 world_tex_p3t2c4_vert.metal（P3T2C4 + 世界空间）：
// 世界空间字形与位图同格式同拓扑，只有片元不同，这条管线由
// Runtime::resolvePipeline 的 fragmentShaderOverride 选出。
// 两者的 varying 名字必须一致——MSL 的 [[stage_in]] 按**字段名**匹配。
//
// texture(0) / sampler(0) 与 metalCommon.h 的 toMetalTextureIndex(set=0,
// binding=0) 一致——RHI 的 uTex 声明为 (set 0, binding 0) 的采样纹理。
#include <metal_stdlib>
using namespace metal;

struct RxFragmentIn
{
    float2 vUV;
    float4 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        texture2d<float> uTex [[texture(0)]],
                        sampler uTexSampler [[sampler(0)]])
{
    // 图集（R8Unorm）里存的不是覆盖率而是距离场：约 0.5 表示该点恰在字形
    // 轮廓上，越大越深入字形内部。由 stbtt_GetGlyphSDF 生成，
    // 见 FontDesc::sdfPadding。转成符号距离：>0 在字形内、<0 在字形外。
    const float d = uTex.sample(uTexSampler, in.vUV).r - 0.5f;

    // 这一行是「一张图集服务所有缩放」的成立条件。
    //
    // fwidth 求的是相邻屏幕像素间 d 的变化量，也就是「一个屏幕像素跨越多少
    // 距离场单位」。用它当 smoothstep 的窗口，抗锯齿过渡宽度就恒等于约一个
    // 屏幕像素，与当前缩放、图集的光栅化精度都无关——放大到十倍不糊，
    // 缩小到十分之一也不出锯齿。
    //
    // 换成固定窗口（例如 smoothstep(-0.1, 0.1, d)）就退化成旧 text_sdf.frag
    // 的行为：某一个特定缩放下勉强可看，其余缩放要么锯齿要么发虚。
    //
    // 下限保护：极端放大时 fwidth 趋于 0，窗口退化会让 smoothstep 变成硬阈值。
    const float w = max(fwidth(d), 1e-5f);
    const float alpha = smoothstep(-w, w, d);

    // rgb 全取顶点色、alpha 乘覆盖率：同一份图集可以画任意颜色的文字。
    return float4(in.vColor.rgb, in.vColor.a * alpha);
}
