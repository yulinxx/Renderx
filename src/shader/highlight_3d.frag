#version 410 core

uniform vec3 uHighlightColor;

out vec4 FragColor;

void main()
{
    FragColor = vec4(uHighlightColor, 0.5);
}