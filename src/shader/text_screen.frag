#version 460 core

in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uFontAtlas;

out vec4 FragColor;

void main()
{
    float alpha = texture(uFontAtlas, vTexCoord).r;
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
