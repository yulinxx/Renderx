// 屏幕空间点图元 / P3C4 —— 顶点阶段
//
// 与 GLSL 版（src/shader/screen_point_p3c4.vert）对应。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float4 aColor [[attribute(1)]];
};

struct RxVertexOut
{
    float4 position [[position]];
    float pointSize [[point_size]];
    float4 vColor;
};

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]])
{
    RxVertexOut out;
    float2 ndc = float2(in.aPos.x / pc.uViewport.x * 2.0 - 1.0,
                        1.0 - in.aPos.y / pc.uViewport.y * 2.0);
    out.vColor = in.aColor;
    out.position = float4(ndc, 0.0, 1.0);
    out.pointSize = pc.uPointSize;
    return out;
}
