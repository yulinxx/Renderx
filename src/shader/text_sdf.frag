#version 460 core

in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uFontAtlas;
uniform float uSdfScale = 4.0;

out vec4 FragColor;

void main()
{
    float dist = texture(uFontAtlas, vTexCoord).r;
    float width = 0.5 / uSdfScale;
    float alpha = smoothstep(0.5 - width, 0.5 + width, dist);
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}