#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 参考框着色器 - 固定在世界坐标中，缩放时会随视图变化
    const char* REFERENCE_BOX_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;    // 投影视图组合矩阵
        uniform float boxWidth;         // 参考框宽度（世界坐标单位）
        uniform float boxHeight;        // 参考框高度（世界坐标单位）

        void main()
        {
            // 应用视图矩阵变换，固定在世界坐标原点附近
            vec3 worldPos = vec3(position.x * boxWidth, position.y * boxHeight, 1.0);
            vec3 clipPos = projectionView * worldPos;

            gl_Position = vec4(clipPos.xy, 0.0, 1.0);
        }
    )";

    const char* REFERENCE_BOX_FS = GLSL_VERSION_STR R"(
        out vec4 fragColor;
        uniform vec4 boxColor;          // 参考框颜色

        void main()
        {
            fragColor = boxColor;
        }
    )";
}