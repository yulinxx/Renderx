#version 460 core

layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform vec2 uViewportSize;

out vec2 vTexCoord;
out vec4 vColor;

void main()
{
    // pixel coord -> NDC, y flip (top-left origin -> GL bottom-left)
    vec2 ndc = vec2(aPosition.x / uViewportSize.x,
                   1.0 - aPosition.y / uViewportSize.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
