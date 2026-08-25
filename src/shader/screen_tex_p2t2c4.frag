// 屏幕空间带纹理 / P2T2C4 —— 纹理与顶点色相乘
#version 330 core

in vec2 vUV;
in vec4 vColor;

uniform sampler2D uTex;

out vec4 frag;

void main()
{
    frag = texture(uTex, vUV) * vColor;
}
