#version 410 core

in vec3 vNormal;

uniform vec3 uLightDir = vec3(0.577, 0.577, 0.577);
uniform vec3 uAmbientColor = vec3(0.2, 0.2, 0.2);
uniform vec3 uDiffuseColor = vec3(0.6, 0.6, 0.8);

out vec4 FragColor;

void main()
{
    vec3 normal = normalize(vNormal);
    float diff = max(dot(normal, uLightDir), 0.0);
    vec3 color = uAmbientColor + diff * uDiffuseColor;
    FragColor = vec4(color, 1.0);
}