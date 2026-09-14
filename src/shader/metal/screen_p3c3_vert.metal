// 屏幕空间 / P3C3 —— 顶点阶段
//
// 与 GLSL 版（src/shader/screen_p3c3.vert）一致：aPos.xy 直接是像素坐标
// （左上原点），在顶点着色器里转 NDC。
//
// Metal 的 NDC 与 OpenGL 同为 y 向上，且视口原点在左上——两者的组合下
// 这里的 y 取反公式与 GLSL 侧完全相同，不需要额外翻转。
// 片元复用 world_p3c3.frag（只吃 vColor，与空间无关）。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float3 aColor [[attribute(1)]];
};

struct RxVertexOut
{
    float4 position [[position]];
    float3 vColor;
};

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]])
{
    RxVertexOut out;
    float2 ndc = float2(in.aPos.x / pc.uViewport.x * 2.0 - 1.0,
                        1.0 - in.aPos.y / pc.uViewport.y * 2.0);
    out.vColor = in.aColor;
    out.position = float4(ndc, 0.0, 1.0);
    return out;
}
