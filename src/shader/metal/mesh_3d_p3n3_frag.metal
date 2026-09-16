// 3D 网格 / P3N3 —— 片段阶段，Blinn-Phong 光照
//
// 与 GLSL 版（src/shader/mesh_3d_p3n3.frag）逐行对应。
// 光照模型：
//   - 环境光：uAmbientColor * uAmbientIntensity
//   - 三个方向光（Key/Fill/Rim）：Lambert 漫反射 + Blinn-Phong 高光
//   - 材质：pc.uMatDiffuse（漫反射）+ pc.uMatAmbient（环境系数）+ pc.uMatSpecular（高光颜色/指数）

#include <metal_stdlib>
using namespace metal;

#include "rx_push_constants.metal"

// 顶点着色器输出
struct RxFragmentIn
{
    float4 position [[position]];
    float3 vNormal;
    float3 vWorldPos;
};

// 与顶点着色器一致的 FrameUniforms 定义。
// 布局约束同 mesh_3d_p3n3_vert.metal / rx_push_constants.metal：
// MSL 标量不共享 float3 尾槽，「vec3+同槽标量」一律用 float4 承载。
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
static_assert(sizeof(FrameUniforms) == 160, "FrameUniforms 必须与 Lighting3DDesc 同为 160 字节");

// 计算一个方向光的贡献
float3 calcDirectionalLight(float3 N, float3 V, float3 L,
                              float3 lightColor, float lightIntensity,
                              float3 matDiffuse, float3 matSpecular, float shininess,
                              bool specularEnabled)
{
    // Lambert 漫反射
    float NdotL = dot(N, L);
    float diffuse = max(NdotL, 0.0);

    float3 diffuseContrib = matDiffuse * lightColor * lightIntensity * diffuse;

    // Blinn-Phong 高光
    float3 specularContrib = float3(0.0);
    if (specularEnabled && shininess > 0.0)
    {
        float3 H = normalize(V + L);  // 半程向量
        float NdotH = max(dot(N, H), 0.0);
        float spec = pow(NdotH, shininess);
        specularContrib = matSpecular * lightColor * lightIntensity * spec;
    }

    return diffuseContrib + specularContrib;
}

fragment float4 fs_main(RxFragmentIn in [[stage_in]],
                        constant RxPushConstants& pc [[buffer(30)]],
                        // set=0/binding=1 → Metal buffer index 17（见
                        // metalCommon.h 的 kMetalBindGroupBufferBase 约定）
                        constant FrameUniforms& frame [[buffer(17)]],
                        bool frontFacing [[front_facing]])
{
    float3 N = normalize(in.vNormal);
    float3 V = normalize(frame.uViewPosMinBrightness.xyz - in.vWorldPos);

    // 从 float4 同槽字段解包：xyz 是向量、w 是标量（uint 标志位按位重解释）
    const bool ambientEnabled = as_type<uint>(frame.uAmbientColorEnabled.w) != 0;
    const float3 ambientColor = frame.uAmbientColorEnabled.xyz;
    const float minBrightness = frame.uViewPosMinBrightness.w;

    // 材质参数
    float3 matDiffuse = pc.uMatDiffuse.rgb;
    float3 matAmbient = pc.uMatAmbient.rgb;  // xyz only
    float3 matSpecular = pc.uMatSpecular.rgb;
    float shininess = pc.uMatSpecular.w;

    // 如果是双面渲染，翻转背面法线。
    // Metal 没有 gl_FrontFacing 内置变量，正面朝向通过片元函数的
    // [[front_facing]] 参数传入；何为正面由管线的 frontFacingWinding 决定。
    if (frame.uDoubleSided != 0 && !frontFacing)
    {
        N = -N;
    }

    // 初始化颜色
    float3 color = float3(0.0);

    // 环境光
    if (ambientEnabled)
    {
        color += matAmbient * ambientColor * frame.uAmbientIntensity;
    }

    // 三个方向光
    if (as_type<uint>(frame.uKeyLight.directionAndEnabled.w) != 0)
    {
        float3 L = normalize(frame.uKeyLight.directionAndEnabled.xyz);
        color += calcDirectionalLight(N, V, L,
                                      frame.uKeyLight.colorAndIntensity.xyz,
                                      frame.uKeyLight.colorAndIntensity.w,
                                      matDiffuse, matSpecular, shininess,
                                      frame.uSpecularEnabled != 0);
    }

    if (as_type<uint>(frame.uFillLight.directionAndEnabled.w) != 0)
    {
        float3 L = normalize(frame.uFillLight.directionAndEnabled.xyz);
        color += calcDirectionalLight(N, V, L,
                                      frame.uFillLight.colorAndIntensity.xyz,
                                      frame.uFillLight.colorAndIntensity.w,
                                      matDiffuse, matSpecular, shininess,
                                      frame.uSpecularEnabled != 0);
    }

    if (as_type<uint>(frame.uRimLight.directionAndEnabled.w) != 0)
    {
        float3 L = normalize(frame.uRimLight.directionAndEnabled.xyz);
        color += calcDirectionalLight(N, V, L,
                                      frame.uRimLight.colorAndIntensity.xyz,
                                      frame.uRimLight.colorAndIntensity.w,
                                      matDiffuse, matSpecular, shininess,
                                      frame.uSpecularEnabled != 0);
    }

    // 最小亮度（避免全黑）
    color = max(color, float3(minBrightness));

    // 曝光
    color *= frame.uExposure;

    // 透明度来自材质 alpha
    return float4(color, pc.uMatDiffuse.a);
}
