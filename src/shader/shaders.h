#pragma once

namespace render::shader {

inline const char* SCENE_2D_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aColor;

uniform mat3 uViewMatrix;

out vec3 vColor;

void main() {
    gl_Position = vec4(uViewMatrix * vec3(aPos.xy, 1.0), 0.0, 1.0);
    vColor = aColor;
}
)";

inline const char* SCENE_2D_FRAG = R"(
#version 460 core
in vec3 vColor;

out vec4 FragColor;

void main() {
    FragColor = vec4(vColor, 1.0);
}
)";

inline const char* OVERLAY_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;

uniform mat3 uViewMatrix;

out vec4 vColor;

void main() {
    gl_Position = vec4(uViewMatrix * vec3(aPos.xy, 1.0), 0.0, 1.0);
    vColor = aColor;
}
)";

inline const char* OVERLAY_FRAG = R"(
#version 460 core
in vec4 vColor;

out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

inline const char* OVERLAY_SCREEN_VERT = R"(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec4 aColor;

uniform vec2 uViewportSize;

out vec4 vColor;

void main() {
    vec2 ndc = (aPos / uViewportSize) * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
    vColor = aColor;
}
)";

inline const char* OVERLAY_SCREEN_FRAG = R"(
#version 460 core
in vec4 vColor;

out vec4 FragColor;

void main() {
    FragColor = vColor;
}
)";

inline const char* BITMAP_VERT = R"(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;

uniform mat3 uViewMatrix;

out vec2 vTexCoord;

void main() {
    gl_Position = vec4(uViewMatrix * vec3(aPos, 1.0), 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

inline const char* BITMAP_FRAG = R"(
#version 460 core
in vec2 vTexCoord;

uniform sampler2D uTexture;

out vec4 FragColor;

void main() {
    FragColor = texture(uTexture, vTexCoord);
}
)";

inline const char* MESH_3D_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;
uniform mat4 uModelMatrix;

out vec3 vNormal;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = uModelMatrix * vec4(aPos, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
    vNormal = mat3(uModelMatrix) * aNormal;
    vWorldPos = worldPos.xyz;
}
)";

inline const char* MESH_3D_FRAG = R"(
#version 460 core
in vec3 vNormal;
in vec3 vWorldPos;

uniform vec3 uLightDir;
uniform vec3 uAmbientColor;
uniform vec3 uDiffuseColor;
uniform vec3 uSpecularColor;
uniform float uShininess;

out vec4 FragColor;

void main() {
    vec3 N = normalize(vNormal);
    vec3 L = normalize(uLightDir);
    vec3 V = normalize(-vWorldPos);
    vec3 R = reflect(-L, N);

    vec3 ambient = uAmbientColor;
    float diff = max(dot(N, L), 0.0);
    vec3 diffuse = uDiffuseColor * diff;
    float spec = pow(max(dot(V, R), 0.0), uShininess);
    vec3 specular = uSpecularColor * spec;

    FragColor = vec4(ambient + diffuse + specular, 1.0);
}
)";

inline const char* MESH_3D_INSTANCED_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;

layout(std140, binding = 0) readonly buffer InstanceData {
    mat4 modelMatrix[];
    uint materialIndex[];
};

uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;

out vec3 vNormal;
out vec3 vWorldPos;

void main() {
    vec4 worldPos = modelMatrix[gl_InstanceID] * vec4(aPos, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
    vNormal = mat3(modelMatrix[gl_InstanceID]) * aNormal;
    vWorldPos = worldPos.xyz;
}
)";

inline const char* TEXT_SDF_VERT = R"(
#version 460 core
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;

uniform mat3 uViewMatrix;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    gl_Position = vec4(uViewMatrix * vec3(aPos, 1.0), 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

inline const char* TEXT_SDF_FRAG = R"(
#version 460 core
in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uFontAtlas;

out vec4 FragColor;

void main() {
    float dist = texture(uFontAtlas, vTexCoord).r;
    float alpha = smoothstep(0.4, 0.6, dist);
    FragColor = vec4(vColor.rgb, vColor.a * alpha);
}
)";

inline const char* HIGHLIGHT_3D_VERT = R"(
#version 460 core
layout(location = 0) in vec3 aPos;

uniform mat4 uViewMatrix;
uniform mat4 uProjMatrix;
uniform mat4 uModelMatrix;

void main() {
    vec4 worldPos = uModelMatrix * vec4(aPos, 1.0);
    gl_Position = uProjMatrix * uViewMatrix * worldPos;
}
)";

inline const char* HIGHLIGHT_3D_FRAG = R"(
#version 460 core

uniform vec4 uHighlightColor;

out vec4 FragColor;

void main() {
    FragColor = uHighlightColor;
}
)";

}
