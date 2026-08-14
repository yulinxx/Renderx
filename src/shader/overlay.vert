#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;

uniform mat3 uViewMatrix;
uniform vec2 uCameraCenter;

out vec4 vColor;

void main()
{
    // 与世界实体（scene_2d.vert）一致：先减去相机中心，再乘 scale-only 视图矩阵。
    // 这样 overlay 与世界实体使用完全相同的变换路径，避免两套矩阵约定不一致。
    vec2 relPos = aPosition.xy - uCameraCenter;
    vec3 pos = uViewMatrix * vec3(relPos, 1.0);
    gl_Position = vec4(pos.xy, aPosition.z, 1.0);
    vColor = aColor;
}
