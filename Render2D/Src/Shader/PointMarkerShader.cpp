#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 屏幕空间固定大小的方形点标记顶点着色器
    // - position: 世界坐标中心点
    // - 通过 gl_VertexID 的奇偶判断当前是方形的哪个角（0,1,2,3 每4个顶点构成一个方块）
    const char* POINT_MARKER_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;
        uniform vec2 viewportSize;
        uniform float markerSize;     // 像素大小
        uniform int   cornerIndex;    // 当前批次的起始角索引（用于统一绘制4角）

        void main()
        {
            // position 为该标记的世界坐标中心点
            vec3 clip = projectionView * vec3(position, 1.0);

            // 根据 gl_VertexID % 4 计算当前角点：
            //   0: (-1,-1)  1: (+1,-1)  2: (+1,+1)  3: (-1,+1)
            int id = gl_VertexID - (gl_VertexID / 4) * 4;
            vec2 corner;
            if (id == 0)      corner = vec2(-1.0, -1.0);
            else if (id == 1) corner = vec2( 1.0, -1.0);
            else if (id == 2) corner = vec2( 1.0,  1.0);
            else               corner = vec2(-1.0,  1.0);

            // 将像素级大小转换为 NDC[-1,1] 空间
            vec2 ndcOffset = corner * (markerSize / viewportSize);
            gl_Position = vec4(clip.xy + ndcOffset, 0.0, 1.0);
        }
    )";

    const char* POINT_MARKER_FS = GLSL_VERSION_STR R"(
        uniform vec4 fillColor;
        out vec4 fragColor;
        void main()
        {
            fragColor = fillColor;
        }
    )";
}