// 世界空间 / P3C4（位置 + RGBA）—— 顶点阶段
//
// 与 GLSL 版（src/shader/world_p3c4.vert）对应。alpha 交给管线的混合状态。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float4 aColor [[attribute(1)]];
};

struct RxVertexOut
{
    float4 position [[position]];
    float4 vColor;
};

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]])
{
    RxVertexOut out;
    out.vColor = in.aColor;
    out.position = pc.uView * float4(in.aPos, 1.0);
    return out;
}
