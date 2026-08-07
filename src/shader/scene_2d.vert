#version 330 core

layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aColor;

uniform mat3 uViewMatrix;
uniform vec2 uCameraCenter;

out vec3 vColor;

void main()
{
    // Camera-relative rendering: subtract camera center per-frame to avoid
    // catastrophic cancellation in the view matrix multiply when both
    // scale*worldPos and translation are large (e.g., zoomed in on large coords).
    // For World2D: uCameraCenter = camera center, uViewMatrix = scale-only.
    // For SceneEnv: uCameraCenter = (0,0), uViewMatrix = full matrix.
    vec2 relPos = aPosition.xy - uCameraCenter;
    vec3 pos = uViewMatrix * vec3(relPos, 1.0);
    gl_Position = vec4(pos.xy, aPosition.z, 1.0);
    vColor = aColor;
}
