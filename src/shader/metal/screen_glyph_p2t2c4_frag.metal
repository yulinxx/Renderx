// 字形四边形 / P2T2C4 —— 图集是 R8 覆盖率，颜色来自顶点（片段阶段）
//
// 与 GLSL 版（src/shader/screen_glyph_p2t2c4.frag）逐行对应。
//
// 顶点阶段复用 screen_tex_p2t2c4_vert.metal（P2T2C4 + 屏幕空间）：
// 字形与位图同格式同拓扑，只有片元不同，这条管线由
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
    // 图集是 R8Unorm，采样得到 (r, 0, 0, 1)：g/b 为 0、a 为 1。
    // 因此**不能**像位图那样直接 sample() * vColor —— 那会把字画成红色。
    // 这个坑与 GLSL 侧完全一样，见 screen_glyph_p2t2c4.frag 的注释。
    const float coverage = uTex.sample(uTexSampler, in.vUV).r;

    // rgb 全取顶点色、alpha 乘覆盖率：同一份图集可以画任意颜色的文字。
    // 覆盖率是 stb_truetype 光栅化出的抗锯齿灰度，直接当 alpha 用即可；
    // 对它做 smoothstep 相当于硬阈值化，反而削掉边缘信息——那是 SDF 版
    // 该做的事（见 world_glyph_sdf_p3t2c4_frag.metal）。
    return float4(in.vColor.rgb, in.vColor.a * coverage);
}
