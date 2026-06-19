#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 控制线顶点着色器
    const char* CONTROL_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        void main()
        {
            vec3 clipPos = projectionView * vec3(position, 1.0);
            gl_Position = vec4(clipPos.xy, 0.0, 1.0);
        }
    )";

    // 控制线片段着色器
    const char* CONTROL_FS = GLSL_VERSION_STR R"(
        out vec4 fragColor;
        uniform vec4 color;
        void main()
        {
            fragColor = color;
        }
    )";
}