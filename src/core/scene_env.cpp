#include "scene_env.h"
#include "../shader/shaders.h"
#include <cstring>
#include <algorithm>

namespace render::core {

bool SceneEnv::initialize(rhi::IDevice* device)
{
    {
        rhi::BufferDesc desc{};
        desc.size       = 1024 * 1024;
        desc.usage      = rhi::BufferUsage::Vertex;
        desc.memory     = rhi::MemoryType::CPU_Visible;
        desc.debugName  = "SceneEnvVB";
        m_vertexBuffer  = device->createBuffer(desc);
        if (m_vertexBuffer == rhi::NullHandle)
            return false;
    }

    {
        rhi::PipelineDesc desc{};
        desc.topology       = rhi::PrimitiveTopology::LineList;
        desc.vertexShader   = shader::SCENE_2D_VERT;
        desc.fragmentShader = shader::SCENE_2D_FRAG;
        desc.computeShader  = nullptr;
        desc.depthTest      = false;
        desc.depthWrite     = false;
        desc.blendEnable    = false;
        desc.srcBlend       = rhi::BlendFactor::Zero;
        desc.dstBlend       = rhi::BlendFactor::Zero;
        desc.depthFunc      = rhi::CompareFunc::Always;
        m_linePipeline      = device->createPipeline(desc);
        if (m_linePipeline == rhi::NullHandle)
            return false;
    }

    {
        rhi::PipelineDesc desc{};
        desc.topology       = rhi::PrimitiveTopology::TriangleList;
        desc.vertexShader   = shader::SCENE_2D_VERT;
        desc.fragmentShader = shader::SCENE_2D_FRAG;
        desc.computeShader  = nullptr;
        desc.depthTest      = false;
        desc.depthWrite     = false;
        desc.blendEnable    = true;
        desc.srcBlend       = rhi::BlendFactor::SrcAlpha;
        desc.dstBlend       = rhi::BlendFactor::OneMinusSrcAlpha;
        desc.depthFunc      = rhi::CompareFunc::Always;
        m_trianglePipeline  = device->createPipeline(desc);
        if (m_trianglePipeline == rhi::NullHandle)
            return false;
    }

    return true;
}

void SceneEnv::shutdown()
{
}

void SceneEnv::setGeometry(const VertexP3C3* vertices, uint32_t vertexCount,
                           const uint32_t* layerOffsets, uint32_t layerCount,
                           const uint32_t* layerColors, const float* lineWidths)
{
    m_vertices.assign(vertices, vertices + vertexCount);

    m_layers.resize(layerCount);
    for (uint32_t i = 0; i < layerCount; ++i) {
        EnvLayer& layer     = m_layers[i];
        layer.firstVertex   = layerOffsets[i];
        layer.vertexCount   = (i + 1 < layerCount)
                                ? (layerOffsets[i + 1] - layerOffsets[i])
                                : (vertexCount - layerOffsets[i]);

        uint32_t col = layerColors[i];
        layer.color[0] = static_cast<float>((col >>  0) & 0xFF) / 255.0f;
        layer.color[1] = static_cast<float>((col >>  8) & 0xFF) / 255.0f;
        layer.color[2] = static_cast<float>((col >> 16) & 0xFF) / 255.0f;
        layer.color[3] = static_cast<float>((col >> 24) & 0xFF) / 255.0f;

        layer.lineWidth   = lineWidths[i];
        layer.asTriangles = (layer.vertexCount % 3 == 0) && (layer.vertexCount > 0);
    }

    m_dirty = true;
}

void SceneEnv::render(rhi::IDevice* device, const float viewMatrix[9])
{
    if (m_layers.empty())
        return;

    if (m_dirty) {
        if (!m_vertices.empty()) {
            device->uploadBuffer(m_vertexBuffer, 0,
                                 m_vertices.size() * sizeof(VertexP3C3),
                                 m_vertices.data());
        }
        m_dirty = false;
    }

    for (const auto& layer : m_layers) {
        if (layer.vertexCount == 0)
            continue;

        rhi::PipelineHandle pipe = layer.asTriangles ? m_trianglePipeline : m_linePipeline;
        device->bindPipeline(pipe);
        device->bindVertexBuffer(0, m_vertexBuffer, layer.firstVertex * sizeof(VertexP3C3));
        device->setLineWidth(layer.lineWidth);
        device->draw(layer.vertexCount, 1, 0, 0);
    }
}

}
