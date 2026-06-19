#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>

namespace render::core {

class SceneEnv
{
public:
    bool initialize(rhi::IDevice* device);
    void shutdown();

    void setGeometry(const VertexP3C3* vertices, uint32_t vertexCount,
                     const uint32_t* layerOffsets, uint32_t layerCount,
                     const uint32_t* layerColors, const float* lineWidths);

    void render(rhi::IDevice* device, const float viewMatrix[9]);

private:
    struct EnvLayer
    {
        uint32_t firstVertex;
        uint32_t vertexCount;
        float    color[4];
        float    lineWidth;
        bool     asTriangles;
    };

    std::vector<VertexP3C3> m_vertices;
    std::vector<EnvLayer>   m_layers;

    rhi::BufferHandle   m_vertexBuffer     = rhi::NullHandle;
    rhi::PipelineHandle m_linePipeline     = rhi::NullHandle;
    rhi::PipelineHandle m_trianglePipeline = rhi::NullHandle;

    bool m_dirty = true;
};

}
