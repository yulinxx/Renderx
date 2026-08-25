// 世界锚定 + 屏幕定尺寸 / P3O2C4（世界锚点 + 像素偏移 + RGBA）
//
// 对应 RenderSpace::WorldPinned：图元跟随平移，但屏幕尺寸不随缩放变化。
// 用于场景内的定尺寸标记——箭头、符号、标注框、引线端点。
//
// 换算原理：先把锚点变换到裁剪空间，再把像素偏移换算成裁剪空间增量。
// 乘 clip.w 是关键：它抵消后续的透视除法，使偏移在 NDC 上恰好等于
// offset 个像素，因此与 uView 里的缩放量无关。
//
// 拾取端必须用同一公式（锚点投影 → 加像素偏移），否则视觉与命中区
// 会随缩放错位。见 Docs/03-渲染主链/新渲染架构.md §15。
//
// layout(location) 必须显式标注：Apple 的 GLSL 编译器在缺省时会乱序
// 分配 attribute slot（见 Docs/Mac渲染.md §9）。
#version 330 core

layout(location = 0) in vec3 aAnchor;
layout(location = 1) in vec2 aOffsetPx;
layout(location = 2) in vec4 aColor;

uniform mat4 uView;
/// 视口像素尺寸（宽, 高），由 Session 每帧写入
uniform vec2 uViewportSize;

out vec4 vColor;

void main()
{
    vColor = aColor;

    vec4 clip = uView * vec4(aAnchor, 1.0);

    // 退化保护：视口尺寸为 0（窗口最小化）时不做偏移，避免除零产生 NaN
    // 顶点——NaN 顶点会让整个图元消失，且没有任何报错。
    if (uViewportSize.x > 0.0 && uViewportSize.y > 0.0)
    {
        clip.xy += aOffsetPx * (2.0 / uViewportSize) * clip.w;
    }

    gl_Position = clip;
}
