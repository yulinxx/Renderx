#version 410 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aTexCoord;

uniform mat3 uViewMatrix;
uniform vec2 uCameraCenter;

out vec2 vTexCoord;

void main()
{
    // Camera-relative rendering: subtract camera center to avoid catastrophic
    // cancellation on large-coordinate DXF scenes (consistent with scene_2d.vert).
    vec2 relPos = aPosition.xy - uCameraCenter;
    vec3 pos = uViewMatrix * vec3(relPos, 1.0);
    gl_Position = vec4(pos.xy, aPosition.z, 1.0);
    vTexCoord = aTexCoord;
}
