#include "ShaderDef.h"
#include "GLVerDef.h"

namespace Shaders
{
    const char* BITMAP_VS = GLSL_VERSION_STR R"(
        layout(location = 0) in vec2 aPos;
        layout(location = 1) in vec2 aUV;
        out vec2 vUV;
        uniform mat3 projectionView;
        void main()
        {
            vUV = aUV;
            vec3 p = projectionView * vec3(aPos.xy, 1.0);
            gl_Position = vec4(p.xy, 0.0, 1.0);
        }
    )";

    const char* BITMAP_FS = GLSL_VERSION_STR R"(
        in vec2 vUV;
        out vec4 FragColor;
        uniform sampler2D uTex;
        void main()
        {
            FragColor = texture(uTex, vUV);
        }
    )";
}