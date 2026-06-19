#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 台面/网格/标尺共用着色器
    // 输入：position = 世界坐标 (x, y)，如果是标尺则传的是屏幕空间像素。
    // uniform screenToNdc: 将"像素坐标"映射到 [-1,1]。对于台面/网格绘制，直接传 projectionView 矩阵。
    // uniform color: 纯色填充 / 描边颜色
    // uniform zDepth: 本次绘制的深度，用于做层叠排序（台面最低，网格次之，外框更高，图元更高，标尺最高）
    const char* RULER_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 screenToNdc; // 把输入坐标 转换 到 clip space
        uniform float zDepth;     // [-1,1]，越大越近
        void main()
        {
            vec3 clip = screenToNdc * vec3(position, 1.0);
            gl_Position = vec4(clip.xy, zDepth, 1.0);
        }
    )";

    const char* RULER_FS = GLSL_VERSION_STR R"(
        uniform vec4 color;
        out vec4 fragColor;
        void main()
        {
            fragColor = color;
        }
    )";
}