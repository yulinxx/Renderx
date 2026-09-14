// 屏幕空间带纹理 / P2T2C4 —— 纹理与顶点色相乘（片段阶段）
//
// 与 GLSL 版（src/shader/screen_tex_p2t2c4.frag）对应。
// texture(0) / sampler(0) 与 metalCommon.h 的 toMetalTextureIndex(set=0,
// binding=0) 一致——RHI 的 uTex 声明为 (set 0, binding 0) 的采样纹理。
struct RxFragmentIn
{
    float2 vUV;
    float4 vColor;
};

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        texture2d<float> uTex [[texture(0)]],
                        sampler uTexSampler [[sampler(0)]])
{
    return uTex.sample(uTexSampler, in.vUV) * in.vColor;
}
