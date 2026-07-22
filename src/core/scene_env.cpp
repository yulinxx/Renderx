#include "scene_env.h"
#include "shader/shaders.h"
#include <cstring>
#include <algorithm>
#include "Log/SyLogger.h"

namespace render::core
{
    bool SceneEnv::initialize(rhi::IDevice* device)
    {
        m_device = device;

        {
            rhi::BufferDesc desc{};
            desc.size = 1024 * 1024;
            desc.usage = rhi::BufferUsage::Vertex;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "SceneEnvVB";
            m_vertexBuffer = device->createBuffer(desc);
            if (m_vertexBuffer == rhi::NullHandle)
                return false;
        }

        {
            rhi::PipelineDesc desc{};
            desc.topology = rhi::PrimitiveTopology::LineList;
            desc.vertexShader = shader::SCENE_2D_VERT;
            desc.fragmentShader = shader::SCENE_2D_FRAG;
            desc.computeShader = nullptr;
            desc.vertexFormat = rhi::VertexFormat::P3C3;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = false;
            desc.srcBlend = rhi::BlendFactor::Zero;
            desc.dstBlend = rhi::BlendFactor::Zero;
            desc.depthFunc = rhi::CompareFunc::Always;
            m_linePipeline = device->createPipeline(desc);
            if (m_linePipeline == rhi::NullHandle)
                return false;
        }

        {
            rhi::PipelineDesc desc{};
            desc.topology = rhi::PrimitiveTopology::TriangleList;
            desc.vertexShader = shader::SCENE_2D_VERT;
            desc.fragmentShader = shader::SCENE_2D_FRAG;
            desc.computeShader = nullptr;
            desc.vertexFormat = rhi::VertexFormat::P3C3;
            desc.depthTest = false;
            desc.depthWrite = false;
            desc.blendEnable = true;
            desc.srcBlend = rhi::BlendFactor::SrcAlpha;
            desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
            desc.depthFunc = rhi::CompareFunc::Always;
            m_trianglePipeline = device->createPipeline(desc);
            if (m_trianglePipeline == rhi::NullHandle)
                return false;
        }

        return true;
    }

    void SceneEnv::shutdown()
    {
        if (m_device)
        {
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = rhi::NullHandle;
            }
            if (m_linePipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_linePipeline);
                m_linePipeline = rhi::NullHandle;
            }
            if (m_trianglePipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_trianglePipeline);
                m_trianglePipeline = rhi::NullHandle;
            }
        }

        m_vertices.clear();
        m_layers.clear();
        m_dirty = true;
        m_device = nullptr;
    }

    void SceneEnv::setGeometry(const VertexP3C3* vertices, uint32_t vertexCount,
        const uint32_t* layerOffsets, uint32_t layerCount,
        const uint32_t* layerColors, const float* lineWidths)
    {
        setGeometryEx(vertices, vertexCount, layerOffsets, layerCount,
            layerColors, lineWidths, nullptr);
    }

    void SceneEnv::setGeometryEx(const VertexP3C3* vertices, uint32_t vertexCount,
        const uint32_t* layerOffsets, uint32_t layerCount,
        const uint32_t* layerColors, const float* lineWidths,
        const bool* pixelFlags)
    {
        m_vertices.assign(vertices, vertices + vertexCount);

        m_layers.resize(layerCount);
        for (uint32_t i = 0; i < layerCount; ++i)
        {
            EnvLayer& layer = m_layers[i];
            layer.firstVertex = layerOffsets[i];
            layer.vertexCount = (i + 1 < layerCount)
                ? (layerOffsets[i + 1] - layerOffsets[i])
                : (vertexCount - layerOffsets[i]);

            uint32_t col = layerColors[i];
            layer.color[0] = static_cast<float>((col >> 0) & 0xFF) / 255.0f;
            layer.color[1] = static_cast<float>((col >> 8) & 0xFF) / 255.0f;
            layer.color[2] = static_cast<float>((col >> 16) & 0xFF) / 255.0f;
            layer.color[3] = static_cast<float>((col >> 24) & 0xFF) / 255.0f;

            layer.lineWidth = lineWidths[i];
            layer.asTriangles = (layer.vertexCount % 3 == 0) && (layer.vertexCount > 0);
            layer.usePixelCoords = pixelFlags ? pixelFlags[i] : false;
        }

        m_dirty = true;
    }

    void SceneEnv::render(rhi::IDevice* device, const float viewMatrix[9])
    {
        render(device, viewMatrix, 0, 0);
    }

    void SceneEnv::render(rhi::IDevice* device, const float viewMatrix[9],
                      uint32_t viewportWidth, uint32_t viewportHeight)
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

    float pixelMatrix[9] = {
        2.0f / float(viewportWidth), 0.0f, -1.0f,
        0.0f, -2.0f / float(viewportHeight), 1.0f,
        0.0f, 0.0f, 1.0f
    };

    for (size_t i = 0; i < m_layers.size(); ++i) {
        const auto& layer = m_layers[i];
        if (layer.vertexCount == 0)
            continue;

        // SY_DEBUGF("SceneEnv::render: layer[%zu]: %u vertices, color=(%.2f,%.2f,%.2f,%.2f), lineWidth=%.2f, pixelCoords=%d, triangles=%d",
        //           i, layer.vertexCount, layer.color[0], layer.color[1], layer.color[2], layer.color[3],
        //           layer.lineWidth, layer.usePixelCoords ? 1 : 0, layer.asTriangles ? 1 : 0);

        rhi::PipelineHandle pipe = layer.asTriangles ? m_trianglePipeline : m_linePipeline;
        device->bindPipeline(pipe);
        
        const float* matrix = layer.usePixelCoords ? pixelMatrix : viewMatrix;
        device->setUniformMatrix3("uViewMatrix", matrix);
        
        device->bindVertexBuffer(0, m_vertexBuffer, layer.firstVertex * sizeof(VertexP3C3));
        device->setLineWidth(layer.lineWidth);
        device->draw(layer.vertexCount, 1, 0, 0);
    }
}
}