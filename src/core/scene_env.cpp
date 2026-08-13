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
            {
                return false;
            }
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
            {
                SY_ERROR("SceneEnv::initialize: line pipeline creation failed");
                return false;
            }
            SY_DEBUGF("SceneEnv::initialize: line pipeline created, handle=%u", m_linePipeline);
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
            {
                SY_ERROR("SceneEnv::initialize: triangle pipeline creation failed");
                return false;
            }
            SY_DEBUGF("SceneEnv::initialize: triangle pipeline created, handle=%u", m_trianglePipeline);
        }

        SY_DEBUGF("SceneEnv::initialize: vertex buffer=%u", m_vertexBuffer);
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

    void SceneEnv::setGeometryEx(const VertexP3C3* vertices,
        uint32_t vertexCount,
        const uint32_t* layerOffsets,
        uint32_t layerCount,
        const uint32_t* layerColors,
        const float* lineWidths,
        const bool* pixelFlags,
        const bool* triangleFlags,
        const float* zDepths)
    {
        m_vertices.assign(vertices, vertices + vertexCount);

        m_layers.resize(layerCount);
        for (uint32_t i = 0; i < layerCount; ++i)
        {
            EnvLayer& layer = m_layers[i];
            layer.firstVertex = layerOffsets[i];
            layer.vertexCount =
                (i + 1 < layerCount) ? (layerOffsets[i + 1] - layerOffsets[i]) : (vertexCount - layerOffsets[i]);

            uint32_t col = layerColors[i];
            layer.color[0] = static_cast<float>((col >> 0) & 0xFF) / 255.0f;
            layer.color[1] = static_cast<float>((col >> 8) & 0xFF) / 255.0f;
            layer.color[2] = static_cast<float>((col >> 16) & 0xFF) / 255.0f;
            layer.color[3] = static_cast<float>((col >> 24) & 0xFF) / 255.0f;

            layer.lineWidth = lineWidths[i];
            layer.zDepth = zDepths ? zDepths[i] : 0.0f;
            layer.asTriangles =
                triangleFlags ? triangleFlags[i] : ((layer.vertexCount % 3 == 0) && (layer.vertexCount > 0));
            layer.usePixelCoords = pixelFlags ? pixelFlags[i] : false;
        }

        m_dirty = true;
    }

    void SceneEnv::setGeometryDirect(const SceneEnvGeometryDesc* desc)
    {
        m_vertices.clear();
        m_layers.clear();

        if (!desc || !desc->layers || desc->layerCount == 0)
        {
            m_dirty = true;
            return;
        }

        m_layers.resize(desc->layerCount);
        m_vertices.reserve(desc->layers[0].vertexCount * desc->layerCount);

        uint32_t offset = 0;
        for (uint32_t i = 0; i < desc->layerCount; ++i)
        {
            const EnvLayerDesc& src = desc->layers[i];
            EnvLayer& layer = m_layers[i];

            layer.firstVertex = offset;
            layer.vertexCount = (src.vertices && src.vertexCount > 0) ? src.vertexCount : 0;

            layer.color[0] = src.color[0];
            layer.color[1] = src.color[1];
            layer.color[2] = src.color[2];
            layer.color[3] = src.color[3];

            layer.lineWidth = src.lineWidth;
            layer.zDepth = src.zDepth;
            layer.asTriangles = src.asTriangles != 0;
            layer.usePixelCoords = src.usePixelCoords != 0;

            for (uint32_t v = 0; v < src.vertexCount; ++v)
            {
                VertexP3C3 vert;
                vert.px = src.vertices[v * 2 + 0];
                vert.py = src.vertices[v * 2 + 1];
                vert.pz = src.zDepth;
                vert.cr = src.color[0];
                vert.cg = src.color[1];
                vert.cb = src.color[2];
                m_vertices.push_back(vert);
            }

            offset += layer.vertexCount;
        }

        m_dirty = true;
    }

    void SceneEnv::render(rhi::IDevice* device, const float viewMatrix[9])
    {
        render(device, viewMatrix, 0, 0);
    }

    void SceneEnv::render(
        rhi::IDevice* device, const float viewMatrix[9], uint32_t viewportWidth, uint32_t viewportHeight)
    {
        // Check if initialized (m_device is set in initialize())
        if (!m_device)
        {
            return;
        }

        // SY_TRACEF("SceneEnv::render: layers=%zu, vertices=%zu, dirty=%d, viewport=%ux%u",
        //     m_layers.size(), m_vertices.size(), m_dirty ? 1 : 0, viewportWidth, viewportHeight);

        if (m_layers.empty())
        {
            return;
        }

        if (m_dirty)
        {
            if (!m_vertices.empty())
            {
                // SY_TRACEF("SceneEnv::render: uploading %zu vertices to GPU", m_vertices.size());
                device->uploadBuffer(m_vertexBuffer, 0, m_vertices.size() * sizeof(VertexP3C3), m_vertices.data());
            }
            m_dirty = false;
        }

        float pixelMatrix[9] = {
            2.0f / float(viewportWidth), 0.0f, 0.0f, 0.0f, -2.0f / float(viewportHeight), 0.0f, -1.0f, 1.0f, 1.0f
        };

        std::vector<size_t> layerIndices(m_layers.size());
        for (size_t i = 0; i < m_layers.size(); ++i)
        {
            layerIndices[i] = i;
        }

        std::sort(layerIndices.begin(), layerIndices.end(), [this](size_t a, size_t b) {
            const EnvLayer& la = m_layers[a];
            const EnvLayer& lb = m_layers[b];
            return la.zDepth < lb.zDepth;
        });

        for (size_t idx : layerIndices)
        {
            const auto& layer = m_layers[idx];
            if (layer.vertexCount == 0)
            {
                continue;
            }

            rhi::PipelineHandle pipe = layer.asTriangles ? m_trianglePipeline : m_linePipeline;

            if (pipe == rhi::NullHandle)
            {
                SY_ERRORF("SceneEnv::render: layer[%zu] pipeline is NullHandle!", idx);
                continue;
            }

            if (m_vertexBuffer == rhi::NullHandle)
            {
                SY_ERROR("SceneEnv::render: vertex buffer is NullHandle!");
                continue;
            }

            // SY_TRACEF("SceneEnv::render: drawing layer[%zu], %u vertices, pipe=%u",
            //           idx, layer.vertexCount, pipe);

            device->bindPipeline(pipe);

            const float* matrix = layer.usePixelCoords ? pixelMatrix : viewMatrix;
            device->setUniformMatrix3("uViewMatrix", matrix);

            // SceneEnv uses world coordinates, not camera-relative
            const float zero2[2] = { 0.0f, 0.0f };
            device->setUniformVec2("uCameraCenter", zero2);

            device->bindVertexBuffer(0, m_vertexBuffer, layer.firstVertex * sizeof(VertexP3C3));
            device->setLineWidth(layer.lineWidth);
            device->draw(layer.vertexCount, 1, 0, 0);
        }
    }
}  // namespace render::core