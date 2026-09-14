// 世界空间带纹理 / P3T2C4 —— 顶点阶段
//
// 与 GLSL 版（src/shader/world_tex_p3t2c4.vert）对应：顶点已含世界坐标，
// 贴图随平移/缩放一起变换。片元复用 screen_tex_p2t2c4.frag。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float2 aUV [[attribute(1)]];
    float4 aColor [[attribute(2)]];
};

struct RxVertexOut
{
    float4 position [[position]];
    float2 vUV;
    float4 vColor;
};

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]])
{
    RxVertexOut out;
    out.vUV = in.aUV;
    out.vColor = in.aColor;
    out.position = pc.uView * float4(in.aPos, 1.0);
    return out;
}
