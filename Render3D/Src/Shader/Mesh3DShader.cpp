#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // ==================== 3D 网格着色器 (Phong 光照) ====================

    const char* MESH3D_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aNormal;

        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        uniform mat3 uNormalMatrix;

        out vec3 vFragPos;
        out vec3 vNormal;

        void main()
        {
            vec4 worldPos = uModel * vec4(aPosition, 1.0);
            vFragPos = worldPos.xyz;
            vNormal = normalize(uNormalMatrix * aNormal);
            gl_Position = uProjection * uView * worldPos;
        }
    )";

    const char* MESH3D_FS = GLSL_VERSION_STR R"(
        in vec3 vFragPos;
        in vec3 vNormal;

        uniform vec3 uLightPos;
        uniform vec3 uViewPos;
        uniform vec3 uLightColor;
        uniform vec3 uObjectColor;
        uniform vec3 uAmbientColor;
        uniform vec3 uSpecularColor;
        uniform float uShininess;
        uniform float uAmbientStrength;

        out vec4 fragColor;

        void main()
        {
            vec3 normal = normalize(vNormal);

            // Ambient
            vec3 ambient = uAmbientStrength * uLightColor * uAmbientColor;

            // Diffuse
            vec3 lightDir = normalize(uLightPos - vFragPos);
            float diff = max(dot(normal, lightDir), 0.0);
            vec3 diffuse = diff * uLightColor * uObjectColor;

            // Specular
            vec3 viewDir = normalize(uViewPos - vFragPos);
            vec3 reflectDir = reflect(-lightDir, normal);
            float spec = pow(max(dot(viewDir, reflectDir), 0.0), uShininess);
            vec3 specular = spec * uLightColor * uSpecularColor;

            vec3 result = ambient + diffuse + specular;
            fragColor = vec4(result, 1.0);
        }
    )";

    // ==================== 网格/参考平面着色器 ====================

    const char* GRID3D_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec3 aPosition;

        uniform mat4 uView;
        uniform mat4 uProjection;

        out vec3 vWorldPos;

        void main()
        {
            vWorldPos = aPosition;
            gl_Position = uProjection * uView * vec4(aPosition, 1.0);
        }
    )";

    const char* GRID3D_FS = GLSL_VERSION_STR R"(
        in vec3 vWorldPos;

        uniform vec3 uGridColor;
        uniform vec3 uAxisColor;
        uniform float uGridSize;
        uniform float uLineWidth;

        out vec4 fragColor;

        void main()
        {
            vec2 coord = vWorldPos.xz / uGridSize;
            vec2 grid = abs(fract(coord - 0.5) - 0.5) / fwidth(coord);
            float line = min(grid.x, grid.y);
            float alpha = 1.0 - min(line, 1.0);

            // X轴 (红色) 和 Z轴 (蓝色) 加粗
            float axisX = abs(vWorldPos.x) < 0.02 ? 1.0 : alpha;
            float axisZ = abs(vWorldPos.z) < 0.02 ? 1.0 : alpha;

            vec3 gridCol = mix(uGridColor, uAxisColor, max(axisX - alpha, axisZ - alpha));
            fragColor = vec4(gridCol, alpha * 0.3);
        }
    )";

    // ==================== 边界框着色器 ====================

    const char* BBOX3D_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec3 aPosition;

        uniform mat4 uView;
        uniform mat4 uProjection;

        void main()
        {
            gl_Position = uProjection * uView * vec4(aPosition, 1.0);
        }
    )";

    const char* BBOX3D_FS = GLSL_VERSION_STR R"(
        uniform vec4 uColor;
        out vec4 fragColor;

        void main()
        {
            fragColor = uColor;
        }
    )";

    // ==================== 选中高亮着色器 (边缘发光) ====================

    const char* HIGHLIGHT3D_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec3 aPosition;
        layout(location = 1) in vec3 aNormal;

        uniform mat4 uModel;
        uniform mat4 uView;
        uniform mat4 uProjection;
        uniform mat3 uNormalMatrix;

        out vec3 vNormal;
        out vec3 vViewPos;

        void main()
        {
            vec4 worldPos = uModel * vec4(aPosition, 1.0);
            vNormal = normalize(uNormalMatrix * aNormal);
            vViewPos = (uView * worldPos).xyz;
            gl_Position = uProjection * uView * worldPos;
        }
    )";

    const char* HIGHLIGHT3D_FS = GLSL_VERSION_STR R"(
        in vec3 vNormal;
        in vec3 vViewPos;

        uniform vec3 uHighlightColor;

        out vec4 fragColor;

        void main()
        {
            // 简单的边缘检测：基于法线与视线方向的夹角
            vec3 viewDir = normalize(-vViewPos);
            float dotProd = max(dot(vNormal, viewDir), 0.0);

            // 边缘区域：法线与视线夹角较大的地方
            float edge = smoothstep(0.2, 0.6, 1.0 - dotProd);

            // 给边缘添加高亮颜色
            vec3 color = uHighlightColor * edge * 2.0;

            // 确保颜色不会过曝
            color = min(color, vec3(1.0));

            fragColor = vec4(color, edge * 0.8);
        }
    )";
} // namespace Shaders