// 3D 网格片元 / P3N3
//
// Blinn-Phong，三方向光（主光/补光/轮廓光）+ 环境项。
// 光照放在 DLL 内而不是烘进顶点色：高光是视角相关的，烘进顶点意味着
// 相机一动就要重传全部顶点（十万面网格每帧数 MB），或者干脆没有高光。
//
// 材质来自 PushConstants 的 3D 段（per-draw），光照来自 FrameUniforms（per-pass）。
// 这个分工的理由见 rx_lighting_3d.glsl 的注释。
//
// **本文件逐项对齐宿主原 m_meshProgram 的片元逻辑**（RenderWidget3D::initPrograms）。
// 迁移的目标是把裸 GL 收进 DLL，不是顺手改画面：四处容易"顺手改好"的地方
// 都刻意保留了原语义，各自的理由写在下面。
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

    // 双面光照取 abs(dot) 而不是"把法线翻向视线"：CAD 导出的网格绕序与法线
    // 都不可靠（见 Lighting3D.h 的说明），abs 让正反面一视同仁；翻法线的做法
    // 在自交/开放曲面上会沿视线方向出现明暗突变。
    float ndotl = (uDoubleSided != 0u) ? abs(dot(normal, lightDir))
                                       : max(dot(normal, lightDir), 0.0);
    vec3 result = uMatDiffuse.rgb * light.color * (light.intensity * ndotl);

    // 高光不按 ndotl > 0 门控：双面模式下 dot 为负的面同样要有高光，
    // 门控会让背面高光整片消失。
    if (uSpecularEnabled != 0u)
    {
        vec3 halfway = normalize(lightDir + viewDir);
        float spec = pow(max(dot(normal, halfway), 0.0), max(uMatShininess, 1.0));
        result += uMatSpecular * light.color * (spec * light.intensity * uSpecularIntensity);
    }
    return result;
}

void main()
{
    // 退化法线兜底：导入网格里存在零长法线，normalize 会得到 NaN，
    // 整个三角面变成黑洞。给一个固定朝向比 NaN 好排查。
    vec3 normal = vNormal;
    if (!(dot(normal, normal) > 1e-6))
    {
        normal = vec3(0.0, 0.0, 1.0);
    }
    normal = normalize(normal);

    // 视线方向同理：相机恰好落在表面上时 uViewPos - vWorldPos 为零向量
    vec3 viewDir = uViewPos - vWorldPos;
    if (!(dot(viewDir, viewDir) > 1e-6))
    {
        viewDir = vec3(0.0, 0.0, 1.0);
    }
    else
    {
        viewDir = normalize(viewDir);
    }

    vec3 color = vec3(0.0);
    if (uAmbientEnabled != 0u)
    {
        color += uMatAmbient * uAmbientColor * uAmbientIntensity;
    }
    color += rxShadeDirectional(uKeyLight, normal, viewDir);
    color += rxShadeDirectional(uFillLight, normal, viewDir);
    color += rxShadeDirectional(uRimLight, normal, viewDir);

    // 顺序是先曝光再抬下限，不能调换：亮度下限的语义是"最终画面不低于这个亮度"，
    // 若先抬下限再乘曝光，曝光 < 1 时下限会被一起压掉，暗腔重新变黑。
    color *= uExposure;
    // 抬成中性灰而不是按材质色抬：材质色乘下限会让深色件的暗面仍旧接近黑，
    // 而这个参数存在的唯一目的就是看清深腔/窄槽的形状。
    color = max(color, vec3(uMinBrightness));

    FragColor = vec4(color, uMatDiffuse.a);
}
