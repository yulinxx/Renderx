// 屏幕空间带纹理 / P2T2C4 —— 顶点阶段
//
// 与 GLSL 版（src/shader/screen_tex_p2t2c4.vert）对应：aPos 是像素坐标。
// 片元复用 screen_tex_p2t2c4.frag。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float2 aPos [[attribute(0)]];
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
    float2 ndc = float2(in.aPos.x / pc.uViewport.x * 2.0 - 1.0,
                        1.0 - in.aPos.y / pc.uViewport.y * 2.0);
    out.vUV = in.aUV;
    out.vColor = in.aColor;
    out.position = float4(ndc, 0.0, 1.0);
    return out;
}
