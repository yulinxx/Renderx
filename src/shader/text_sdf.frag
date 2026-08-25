// SDF 字形片段着色器
//
// uSdfScale 取自共用 pushConstant 块（rx_push_constants.glsl）：
// 此前它是带初始化值的独立 uniform（`uniform float uSdfScale = 4.0;`），
// 而 GL 的 uniform 初始化值在不同驱动上生效时机并不一致，且 Vulkan/Metal
// 没有等价语义——统一走 pushConstant 后三个后端行为一致。
#version 410 core

#include "rx_push_constants.glsl"

in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uFontAtlas;

out vec4 FragColor;

void main()
{
    float dist = texture(uFontAtlas, vTexCoord).r;
    float width = 0.5 / uSdfScale;
    float alpha = smoothstep(0.5 - width, 0.5 + width, dist);
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
