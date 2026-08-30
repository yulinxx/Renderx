// 3D 网格片元 / P3N3
//
// Blinn-Phong，三方向光（主光/补光/轮廓光）+ 环境项。
// 光照放在 DLL 内而不是烘进顶点色：高光是视角相关的，烘进顶点意味着
// 相机一动就要重传全部顶点（十万面网格每帧数 MB），或者干脆没有高光。
//
// 材质来自 PushConstants 的 3D 段（per-draw），光照来自 FrameUniforms（per-pass）。
// 这个分工的理由见 rx_lighting_3d.glsl 的注释。
#version 330 core

#include "rx_push_constants.glsl"
#include "rx_lighting_3d.glsl"

in vec3 vWorldPos;
in vec3 vNormal;

out vec4 FragColor;

/// 单个方向光的漫反射 + 高光贡献。enabled 为 0 时整条支路跳过。
vec3 rxShadeDirectional(RxDirectionalLight light, vec3 normal, vec3 viewDir)
{
    if (light.enabled == 0u)
    {
        return vec3(0.0);
    }

    vec3 lightDir = normalize(light.direction);
    float ndotl = max(dot(normal, lightDir), 0.0);
    vec3 result = uMatDiffuse.rgb * light.color * (light.intensity * ndotl);

    // 背面不算高光：ndotl 为 0 时半向量没有物理意义，
    // 仍然计算会在轮廓处出现一条亮边。
    if (uSpecularEnabled != 0u && ndotl > 0.0)
    {
        vec3 halfway = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfway), 0.0), max(uMatShininess, 1.0));
        result += uMatSpecular * light.color * (spec * light.intensity * uSpecularIntensity);
    }
    return result;
}

void main()
{
    vec3 normal = normalize(vNormal);
    vec3 viewDir = normalize(uViewPos - vWorldPos);

    // 双面光照：开放曲面（未闭合网格）的背面法线朝里，不翻转就是纯黑一片。
    if (uDoubleSided != 0u && dot(normal, viewDir) < 0.0)
    {
        normal = -normal;
    }

    vec3 color = vec3(0.0);
    if (uAmbientEnabled != 0u)
    {
        color += uMatAmbient * uAmbientColor * uAmbientIntensity;
    }
    color += rxShadeDirectional(uKeyLight, normal, viewDir);
    color += rxShadeDirectional(uFillLight, normal, viewDir);
    color += rxShadeDirectional(uRimLight, normal, viewDir);

    // 亮度下限：完全背光的面若纯黑，形状就完全看不出来，CAD 场景不可接受。
    // 按材质色而非白色抬升，避免暗面变灰。
    color = max(color, uMatDiffuse.rgb * uMinBrightness);
    color *= uExposure;

    FragColor = vec4(color, uMatDiffuse.a);
}
