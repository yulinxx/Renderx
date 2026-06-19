#include "batch_queue.h"
#include "render_world.h"
#include <cstring>
#include <algorithm>

namespace render {
namespace core {

void BatchQueue::initialize(rhi::IDevice* device) {
    m_device = device;
    m_mergedVertices.reserve(4096);
    m_indirectCmds.reserve(512);
    m_batches.reserve(PRIMITIVE_TYPE_COUNT);
    buildPipelines(device);
}

void BatchQueue::shutdown() {
    m_mergedVertices.clear();
    m_indirectCmds.clear();
    m_batches.clear();

    if (m_vertexBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_vertexBuffer);
        m_vertexBuffer = rhi::NullHandle;
    }
    if (m_indirectBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_indirectBuffer);
        m_indirectBuffer = rhi::NullHandle;
    }
    for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i) {
        if (m_pipelines[i] != rhi::NullHandle) {
            m_device->destroyPipeline(m_pipelines[i]);
            m_pipelines[i] = rhi::NullHandle;
        }
    }

    m_vertexBufferCapacity = 0;
    m_indirectBufferCapacity = 0;
    m_device = nullptr;
}

void BatchQueue::submit(const uint32_t* visibleIndices, uint32_t count,
                        const RenderWorld& world) {
    m_mergedVertices.clear();
    m_indirectCmds.clear();
    m_batches.clear();

    if (count == 0) {
        m_dirty = true;
        return;
    }

    struct SortEntry {
        uint32_t visibleIdx;
        uint16_t primitiveType;
        uint16_t materialIndex;
    };

    std::vector<SortEntry> sorted(count);
    const auto* entries = world.getEntityEntries();

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = visibleIndices[i];
        sorted[i].visibleIdx   = idx;
        sorted[i].primitiveType  = entries[idx].primitiveType;
        sorted[i].materialIndex  = entries[idx].materialIndex;
    }

    std::sort(sorted.begin(), sorted.end(),
              [](const SortEntry& a, const SortEntry& b) {
                  if (a.primitiveType != b.primitiveType)
                      return a.primitiveType < b.primitiveType;
                  return a.materialIndex < b.materialIndex;
              });

    const VertexP3C3* worldVerts = world.getVertexData();
    const auto* materials = world.getMaterials();

    PrimitiveType currentType = static_cast<PrimitiveType>(sorted[0].primitiveType);
    uint16_t currentMaterial = sorted[0].materialIndex;
    uint32_t batchFirstVertex = 0;
    uint32_t batchFirstIndirect = 0;
    uint32_t batchIndirectCount = 0;

    auto flushBatch = [&]() {
        if (batchIndirectCount > 0) {
            Batch b;
            b.type          = currentType;
            b.firstVertex   = batchFirstVertex;
            b.vertexCount   = static_cast<uint32_t>(m_mergedVertices.size()) - batchFirstVertex;
            b.firstIndirect = batchFirstIndirect;
            b.indirectCount = batchIndirectCount;
            b.lineWidth     = materials[currentMaterial].desc.lineWidth;
            m_batches.push_back(b);
        }
    };

    for (uint32_t i = 0; i < count; ++i) {
        uint32_t idx = sorted[i].visibleIdx;
        PrimitiveType type = static_cast<PrimitiveType>(sorted[i].primitiveType);
        uint16_t matIdx = sorted[i].materialIndex;

        if (type != currentType) {
            flushBatch();
            currentType = type;
            currentMaterial = matIdx;
            batchFirstVertex = static_cast<uint32_t>(m_mergedVertices.size());
            batchFirstIndirect = static_cast<uint32_t>(m_indirectCmds.size());
            batchIndirectCount = 0;
        } else if (matIdx != currentMaterial) {
            flushBatch();
            currentMaterial = matIdx;
            batchFirstVertex = static_cast<uint32_t>(m_mergedVertices.size());
            batchFirstIndirect = static_cast<uint32_t>(m_indirectCmds.size());
            batchIndirectCount = 0;
        }

        const auto& e = entries[idx];
        DrawIndirectCmd cmd;
        cmd.vertexCount  = e.vertexCount;
        cmd.instanceCount = 1;
        cmd.firstVertex  = static_cast<uint32_t>(m_mergedVertices.size());
        cmd.baseInstance  = 0;
        m_indirectCmds.push_back(cmd);

        m_mergedVertices.insert(m_mergedVertices.end(),
                                worldVerts + e.vertexOffset,
                                worldVerts + e.vertexOffset + e.vertexCount);
        batchIndirectCount++;
    }

    flushBatch();
    m_dirty = true;
}

void BatchQueue::render(rhi::IDevice* device) {
    if (m_mergedVertices.empty() && m_indirectCmds.empty())
        return;

    ensureVertexCapacity(static_cast<uint32_t>(m_mergedVertices.size()));
    ensureIndirectCapacity(static_cast<uint32_t>(m_indirectCmds.size()));

    device->uploadBuffer(m_vertexBuffer, 0,
                         m_mergedVertices.size() * sizeof(VertexP3C3),
                         m_mergedVertices.data());
    device->uploadBuffer(m_indirectBuffer, 0,
                         m_indirectCmds.size() * sizeof(DrawIndirectCmd),
                         m_indirectCmds.data());

    for (const Batch& batch : m_batches) {
        device->bindPipeline(m_pipelines[static_cast<uint32_t>(batch.type)]);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 batch.firstVertex * sizeof(VertexP3C3));
        device->setLineWidth(batch.lineWidth);
        device->drawIndirect(m_indirectBuffer,
                             batch.firstIndirect * sizeof(DrawIndirectCmd),
                             batch.indirectCount,
                             sizeof(DrawIndirectCmd));
    }

    m_dirty = false;
}

void BatchQueue::ensureVertexCapacity(uint32_t vertexCount) {
    if (vertexCount <= m_vertexBufferCapacity)
        return;

    uint32_t newCap = m_vertexBufferCapacity;
    if (newCap == 0) newCap = 4096;
    while (newCap < vertexCount) newCap *= 2;

    if (m_vertexBuffer != rhi::NullHandle)
        m_device->destroyBuffer(m_vertexBuffer);

    rhi::BufferDesc desc;
    desc.size       = newCap * sizeof(VertexP3C3);
    desc.usage      = rhi::BufferUsage::Vertex;
    desc.memory     = rhi::MemoryType::GPU_Only;
    desc.debugName  = "BatchQueue_VB";

    m_vertexBuffer = m_device->createBuffer(desc);
    m_vertexBufferCapacity = newCap;
}

void BatchQueue::ensureIndirectCapacity(uint32_t cmdCount) {
    if (cmdCount <= m_indirectBufferCapacity)
        return;

    uint32_t newCap = m_indirectBufferCapacity;
    if (newCap == 0) newCap = 512;
    while (newCap < cmdCount) newCap *= 2;

    if (m_indirectBuffer != rhi::NullHandle)
        m_device->destroyBuffer(m_indirectBuffer);

    rhi::BufferDesc desc;
    desc.size       = newCap * sizeof(DrawIndirectCmd);
    desc.usage      = rhi::BufferUsage::Indirect;
    desc.memory     = rhi::MemoryType::GPU_Only;
    desc.debugName  = "BatchQueue_Indirect";

    m_indirectBuffer = m_device->createBuffer(desc);
    m_indirectBufferCapacity = newCap;
}

void BatchQueue::buildPipelines(rhi::IDevice* device) {
    static const char* kVertexShader = "passthrough_vert";
    static const char* kFragmentShader = "passthrough_frag";

    rhi::PrimitiveTopology topoMap[PRIMITIVE_TYPE_COUNT] = {
        rhi::PrimitiveTopology::PointList,
        rhi::PrimitiveTopology::LineList,
        rhi::PrimitiveTopology::LineStrip,
        rhi::PrimitiveTopology::LineLoop,
        rhi::PrimitiveTopology::TriangleList,
        rhi::PrimitiveTopology::TriangleStrip,
        rhi::PrimitiveTopology::TriangleFan,
    };

    for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i) {
        rhi::PipelineDesc desc;
        desc.topology       = topoMap[i];
        desc.vertexShader   = kVertexShader;
        desc.fragmentShader = kFragmentShader;
        desc.computeShader  = nullptr;
        desc.depthTest      = false;
        desc.depthWrite     = false;
        desc.blendEnable    = true;
        desc.srcBlend       = rhi::BlendFactor::SrcAlpha;
        desc.dstBlend       = rhi::BlendFactor::OneMinusSrcAlpha;
        desc.depthFunc      = rhi::CompareFunc::Always;

        m_pipelines[i] = device->createPipeline(desc);
    }
}

} // namespace core
} // namespace render
