// 3D 网格 / P3N3（位置 + 法线）
//
// 顶点已是世界坐标：宿主的网格顶点本来就存世界空间，没有 per-draw 的
// model 矩阵，因此这里不做任何模型变换，法线也直接透传。
// 若将来引入实例化，模型矩阵应作为逐实例顶点属性进来，而不是回退成 uniform。
#version 330 core

#include "rx_push_constants.glsl"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

/// 片元需要世界坐标算视线方向（高光是视角相关的）
out vec3 vWorldPos;
out vec3 vNormal;

void main()
{
    vWorldPos = aPos;
    vNormal = aNormal;
    // uView 是宿主传入的 proj * view 合并矩阵，见 DrawPacket::viewMatrix
    gl_Position = uView * vec4(aPos, 1.0);
}
