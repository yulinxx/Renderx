// 世界空间 / P3C3 —— 不透明输出
#version 330 core

in vec3 vColor;

out vec4 frag;

void main()
{
    frag = vec4(vColor, 1.0);
}
