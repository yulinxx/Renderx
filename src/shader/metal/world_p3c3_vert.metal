// 世界空间 / P3C3（位置 + RGB 颜色）—— 顶点阶段
//
// 与 GLSL 版（src/shader/world_p3c3.vert）逐行对应：位置经 uView 变换。
// 属性槽位必须与 MTLVertexDescriptor 的 index 一致（MetalDevice::
// createGraphicsPipeline 用 VertexAttribute::location 作为 attributes 下标）。
//
// buffer(30) 是 pushConstant 专用槽位，与 metalCommon.h 的
// kMetalPushConstantIndex 一致；刻意避开顶点缓冲槽 0..3。
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
    out.vColor = in.aColor;
    out.position = pc.uView * float4(in.aPos, 1.0);
    return out;
}
