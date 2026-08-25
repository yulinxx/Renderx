// 屏幕空间 / P3C4 —— 保留 alpha
#version 330 core

in vec4 vColor;

out vec4 frag;

void main()
{
    frag = vColor;
}
