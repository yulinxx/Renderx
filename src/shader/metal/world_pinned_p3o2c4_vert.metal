// 世界锚定 + 屏幕定尺寸 / P3O2C4 —— 顶点阶段
//
// 与 GLSL 版（src/shader/world_pinned_p3o2c4.vert）对应：先把锚点变换到
// 裁剪空间，再把像素偏移换算成裁剪空间增量。乘 clip.w 抵消后续透视除法，
// 使偏移在 NDC 上恰好等于 offset 个像素，与 uView 的缩放量无关。
//
// 拾取端必须用同一公式，否则视觉与命中区会随缩放错位。
#include "rx_push_constants.metal"

struct RxVertexIn
{
    float3 aAnchor [[attribute(0)]];
    float2 aOffsetPx [[attribute(1)]];
    float4 aColor [[attribute(2)]];
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

    float4 clip = pc.uView * float4(in.aAnchor, 1.0);
    // 退化保护：视口尺寸为 0（窗口最小化）时不做偏移，避免除零产生 NaN
    // 顶点——NaN 顶点会让整个图元消失，且没有任何报错。
    if (pc.uViewport.x > 0.0 && pc.uViewport.y > 0.0)
    {
        clip.xy += in.aOffsetPx * (2.0 / pc.uViewport) * clip.w;
    }
    out.position = clip;
    return out;
}
