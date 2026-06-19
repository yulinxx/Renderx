#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 线段着色器
    const char* LINE_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView; // 投影视图组合矩阵
        void main()
        {
            vec3 clipPos = projectionView * vec3(position, 1.0);
            gl_Position = vec4(clipPos.xy, 0.0, 1.0);
        }
    )";

    const char* LINE_FS = GLSL_VERSION_STR R"(
        uniform vec4 color;
        out vec4 fragColor;
        void main()
        {
            fragColor = color;
        }
    )";
}