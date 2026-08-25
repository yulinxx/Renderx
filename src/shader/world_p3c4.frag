// 世界空间 / P3C4 —— 保留 alpha，交给管线的混合状态处理
#version 330 core

in vec4 vColor;

out vec4 frag;

void main()
{
    frag = vColor;
}
