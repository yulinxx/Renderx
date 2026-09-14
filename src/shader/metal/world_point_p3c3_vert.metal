// 世界空间点图元 / P3C3 —— 顶点阶段
//
// 与 GLSL 版（src/shader/world_point_p3c3.vert）对应。点尺寸是**像素**，
// 不随缩放变化，因此只能走 [[point_size]] 而不能用顶点几何表达。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float3 aColor [[attribute(1)]];
};

struct RxVertexOut
{
    float4 position [[position]];
    float pointSize [[point_size]];
    float3 vColor;
};

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]])
{
    RxVertexOut out;
    out.vColor = in.aColor;
    out.position = pc.uView * float4(in.aPos, 1.0);
    out.pointSize = pc.uPointSize;
    return out;
}
