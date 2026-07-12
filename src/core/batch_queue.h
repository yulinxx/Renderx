#pragma once

#include "render/render_types.h"
#include "rhi/rhi_device.h"
#include <vector>
#include <cstdint>
#include <algorithm>

namespace render {
namespace core {

class RenderWorld;

class BatchQueue {
public:
    void initialize(rhi::IDevice* device);
    void shutdown();

    void submit(const uint32_t* visibleIndices, uint32_t count,
                const RenderWorld& world);

    void render(rhi::IDevice* device);

private:
    struct Batch {
        PrimitiveType type;
        uint32_t      firstVertex;
        uint32_t      vertexCount;
        uint32_t      firstIndirect;
        uint32_t      indirectCount;
        float         lineWidth;
    };

    std::vector<VertexP3C3>     m_mergedVertices;
    std::vector<DrawIndirectCmd> m_indirectCmds;
    std::vector<Batch>          m_batches;

    rhi::IDevice*       m_device             = nullptr;
    rhi::BufferHandle   m_vertexBuffer       = rhi::NullHandle;
    rhi::BufferHandle   m_indirectBuffer     = rhi::NullHandle;
    rhi::PipelineHandle m_pipelines[7]       = {};
    uint32_t            m_vertexBufferCapacity = 0;
    uint32_t            m_indirectBufferCapacity = 0;
    bool                m_dirty              = false;

    void ensureVertexCapacity(uint32_t vertexCount);
    void ensureIndirectCapacity(uint32_t cmdCount);
    void buildPipelines(rhi::IDevice* device);
};

} // namespace core
} // namespace render
