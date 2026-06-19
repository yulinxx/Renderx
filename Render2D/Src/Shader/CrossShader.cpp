#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 十字光标着色器
    const char* CROSS_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;    // 投影视图组合矩阵
        uniform vec2 mousePos;          // 鼠标位置
        uniform vec2 viewportSize;      // 视口大小

        void main()
        {
            // 将鼠标位置转换到裁剪空间
            vec3 mouseClipPos = projectionView * vec3(mousePos, 1.0);

            float crossPixels = length(position);
            float normalizeFactor = (crossPixels > 0.0) ? crossPixels / (min(viewportSize.x, viewportSize.y) * 0.5) : 0.0;
            vec2 screenOffset = normalize(position) * normalizeFactor;

            // 组合得到最终的裁剪空间位置
            gl_Position = vec4(mouseClipPos.xy + screenOffset, 0.0, 1.0);
        }
    )";

    const char* CROSS_FS = GLSL_VERSION_STR R"(
        out vec4 fragColor;
        void main()
        {
            fragColor = vec4(0.0, 0.0, 1.0, 1.0);
        }
    )";
}