#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    // 捕捉标记着色器 — 屏幕空间固定像素大小
    const char* SNAP_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 position;
        uniform mat3 projectionView;    // 投影视图组合矩阵
        uniform vec2 snapWorldPos;      // 捕捉点的世界坐标
        uniform vec2 viewportSize;      // 视口大小（像素）
        uniform float markerSize;       // 标记像素大小

        void main()
        {
            // 将捕捉点世界坐标转换到裁剪空间
            vec3 snapClip = projectionView * vec3(snapWorldPos, 1.0);

            // 按视口大小将顶点归一化到屏幕空间偏移
            float px = abs(position.x) > 0.001 ? position.x : 0.0;
            float py = abs(position.y) > 0.001 ? position.y : 0.0;
            float len = length(vec2(px, py));
            float scale = (len > 0.001) ? (markerSize / len) / min(viewportSize.x, viewportSize.y) : 0.0;
            vec2 screenOffset = position * scale;

            gl_Position = vec4(snapClip.xy + screenOffset, 0.0, 1.0);
        }
    )";

    const char* SNAP_FS = GLSL_VERSION_STR R"(
        uniform vec4 markerColor;
        out vec4 fragColor;
        void main()
        {
            fragColor = markerColor;
        }
    )";
}