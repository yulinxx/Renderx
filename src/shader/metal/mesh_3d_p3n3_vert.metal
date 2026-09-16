// 3D 网格 / P3N3（位置 + 法线）—— 顶点阶段
//
// 与 GLSL 版（src/shader/mesh_3d_p3n3.vert）逐行对应：
// 顶点是世界坐标（宿主已预变换），直接用 uView 做 MVP 变换。
// 属性槽位必须与 MTLVertexDescriptor 的 index 一致。

#include "rx_push_constants.metal"

// 3D 顶点输入：位置 + 法线
struct RxVertexIn
{
    float3 aPos [[attribute(0)]];
    float3 aNormal [[attribute(1)]];
};

// 3D 顶点输出：裁剪空间位置 + 法线（世界空间）
struct RxVertexOut
{
    float4 position [[position]];
    float3 vNormal;     // 世界空间法线
    float3 vWorldPos;  // 世界空间位置
};

// FrameUniforms：光照参数块（binding 1）
// 必须与 C++ Lighting3DDesc（renderx.h）/ rx_lighting_3d.glsl 的 std140
// 布局逐字节一致。MSL 布局规则（见 rx_push_constants.metal 头注释）：
// float3 是 16 字节 simd 类型，**紧随的标量不会被打包进它的尾槽**（会被
// 推到下一个 16 边界）。因此 std140 的「vec3 + 同槽标量」一律用 float4
// 承载（xyz=向量、w=标量）；uint 标志位用 as_type<uint>(.w) 按位取回。
struct RxDirectionalLight
{
    float4 directionAndEnabled;  // offset 0：xyz=direction，w=enabled(uint 位模式)
    float4 colorAndIntensity;    // offset 16：xyz=color，w=intensity
};
static_assert(sizeof(RxDirectionalLight) == 32, "RxDirectionalLight 必须 32 字节");

struct FrameUniforms
{
    float4 uAmbientColorEnabled;   // offset 0：xyz=ambientColor，w=ambientEnabled
    float uAmbientIntensity;       // offset 16
    uint uDoubleSided;             // offset 20
    uint uSpecularEnabled;         // offset 24
    float uSpecularIntensity;      // offset 28

    RxDirectionalLight uKeyLight;   // offset 32
    RxDirectionalLight uFillLight;  // offset 64
    RxDirectionalLight uRimLight;   // offset 96

    float4 uViewPosMinBrightness;  // offset 128：xyz=viewPos，w=minBrightness
    float uExposure;               // offset 144
    float uLightingPad0[3];        // offset 148-159：尾占位（4 对齐，不能用 float3）
};
// 钉死与 Lighting3DDesc 的 160 字节约定（renderx.h 有同名 static_assert）
static_assert(sizeof(FrameUniforms) == 160, "FrameUniforms 必须与 Lighting3DDesc 同为 160 字节");

vertex RxVertexOut vs_main(RxVertexIn in [[stage_in]],
                           constant RxPushConstants& pc [[buffer(30)]],
                           // bindGroup 的 buffer 在 Metal 表上按
                           // 16 + set*16 + binding 排布（metalCommon.h
                           // kMetalBindGroupBufferBase）：FrameUniforms 是
                           // set=0/binding=1 → index 17，不能照抄 GLSL 的 binding 1。
                           constant FrameUniforms& frame [[buffer(17)]])
{
    RxVertexOut out;

    // 顶点位置：世界坐标 -> 裁剪空间
    float4 worldPos = float4(in.aPos, 1.0);
    out.position = pc.uView * worldPos;
    out.vWorldPos = worldPos.xyz;

    // 法线：如果是世界空间顶点，法线也直接用（假设没有非均匀缩放）
    // 如果需要模型矩阵，这里应该用 inverse transpose
    out.vNormal = in.aNormal;

    return out;
}
