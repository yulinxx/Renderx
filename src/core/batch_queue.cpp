#include "batch_queue.h"
#include "command_encoder.h"
#include "render_world.h"
#include <cstring>
#include <algorithm>
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        void BatchQueue::initialize(rhi::IDevice* device)
        {
            m_device = device;
            m_indirectCmds.reserve(512);
            m_batches.reserve(PRIMITIVE_TYPE_COUNT);
            m_dirtyRanges.reserve(64);
            buildPipelines(device);

            rhi::BufferDesc vbDesc;
            vbDesc.size = 1024 * 1024 * sizeof(VertexP3C3);
            vbDesc.usage = rhi::BufferUsage::Vertex;
            vbDesc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            vbDesc.debugName = "BatchQueue_VertexBuffer";
            m_vertexBuffer = device->createBuffer(vbDesc);
            m_vertexBufferCapacity = 1024 * 1024;
        }

        void BatchQueue::shutdown()
        {
            m_indirectCmds.clear();
            m_batches.clear();
            m_dirtyRanges.clear();
            m_lastVisibleIndices.clear();

            if (m_indirectBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_indirectBuffer);
                m_indirectBuffer = rhi::NullHandle;
            }
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = rhi::NullHandle;
            }
            for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i)
            {
                if (m_pipelines[i] != rhi::NullHandle)
                {
                    m_device->destroyPipeline(m_pipelines[i]);
                    m_pipelines[i] = rhi::NullHandle;
                }
            }

            m_indirectBufferCapacity = 0;
            m_vertexBufferCapacity = 0;
            m_dirty = false;
            m_viewChanged = false;
            m_lastVisibleCount = 0;
            m_lastGeneration = 0;
            m_device = nullptr;
        }

        void BatchQueue::submit(const uint32_t* visibleIndices, uint32_t count,
            const RenderWorld& world)
        {
            if (count == 0)
            {
                m_indirectCmds.clear();
                m_batches.clear();
                m_lastVisibleCount = 0;
                m_lastVisibleIndices.clear();
                m_dirty = true;
                return;
            }

            bool needsRebuild = m_lastVisibleCount != count;

            if (!needsRebuild && count == m_lastVisibleIndices.size())
            {
                for (uint32_t i = 0; i < count; i++)
                {
                    if (visibleIndices[i] != m_lastVisibleIndices[i])
                    {
                        needsRebuild = true;
                        break;
                    }
                }
            }

            uint32_t currentGen = world.getGeneration();
            if (currentGen != m_lastGeneration)
            {
                needsRebuild = true;
                m_lastGeneration = currentGen;
            }

            if (!needsRebuild)
            {
                // SY_INFOF("BatchQueue::submit: no rebuild needed (lastCount=%u, count=%u, lastGen=%u, curGen=%u)",
                //     m_lastVisibleCount, count, m_lastGeneration, world.getGeneration());

                m_dirtyRanges.clear();
                const auto* entries = world.getEntityEntries();
                for (uint32_t i = 0; i < count; i++)
                {
                    uint32_t denseIdx = m_lastVisibleIndices[i];
                    if (entries[denseIdx].dirty)
                    {
                        m_dirtyRanges.push_back({ i, 1 });
                    }
                }
                if (!m_dirtyRanges.empty())
                {
                    m_dirty = true;
                    mergeDirtyRanges();
                }
                return;
            }

            m_dirtyRanges.clear();
            m_indirectCmds.clear();
            m_batches.clear();

            // SY_INFOF("BatchQueue::submit: REBUILD path, count=%u, entries=%zu", count, world.getEntityCount());

            struct SortEntry
            {
                uint32_t visibleIdx;
                uint16_t primitiveType;
                uint16_t materialIndex;
            };

            std::vector<SortEntry> sorted(count);
            const auto* entries = world.getEntityEntries();

            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t idx = visibleIndices[i];
                sorted[i].visibleIdx = idx;
                sorted[i].primitiveType = entries[idx].primitiveType;
                sorted[i].materialIndex = entries[idx].materialIndex;
            }

            std::sort(sorted.begin(), sorted.end(),
                [](const SortEntry& a, const SortEntry& b) {
                    if (a.primitiveType != b.primitiveType)
                        return a.primitiveType < b.primitiveType;
                    return a.materialIndex < b.materialIndex;
                });

            const auto* materials = world.getMaterials();

            if (count == 0)
            {
                m_dirty = true;
                return;
            }

            PrimitiveType currentType = static_cast<PrimitiveType>(sorted[0].primitiveType);
            uint16_t currentMaterial = sorted[0].materialIndex;
            uint32_t batchFirstIndirect = 0;
            uint32_t batchIndirectCount = 0;

            auto flushBatch = [&]() {
                if (batchIndirectCount > 0)
                {
                    Batch b;
                    b.type = currentType;
                    b.firstIndirect = batchFirstIndirect;
                    b.indirectCount = batchIndirectCount;
                    if (currentMaterial < world.getMaterialCount())
                    {
                        b.lineWidth = materials[currentMaterial].desc.lineWidth;
                    }
                    else
                    {
                        b.lineWidth = 1.0f;
                    }
                    b.materialIndex = currentMaterial;
                    m_batches.push_back(b);
                }
                };

            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t idx = sorted[i].visibleIdx;
                PrimitiveType type = static_cast<PrimitiveType>(sorted[i].primitiveType);
                uint16_t matIdx = sorted[i].materialIndex;

                if (type != currentType)
                {
                    flushBatch();
                    currentType = type;
                    currentMaterial = matIdx;
                    batchFirstIndirect = static_cast<uint32_t>(m_indirectCmds.size());
                    batchIndirectCount = 0;
                }
                else if (matIdx != currentMaterial)
                {
                    flushBatch();
                    currentMaterial = matIdx;
                    batchFirstIndirect = static_cast<uint32_t>(m_indirectCmds.size());
                    batchIndirectCount = 0;
                }

                const auto& e = entries[idx];
                DrawIndirectCmd cmd;
                cmd.vertexCount = e.vertexCount;
                cmd.instanceCount = 1;
                cmd.firstVertex = e.vertexOffset;
                cmd.baseInstance = 0;
                m_indirectCmds.push_back(cmd);
                batchIndirectCount++;
            }

            flushBatch();

            // SY_INFOF("BatchQueue::submit: count=%u, indirectCmds=%zu, batches=%zu, dirty=%d",
            //     count, m_indirectCmds.size(), m_batches.size(), m_dirty ? 1 : 0);

            m_lastVisibleCount = count;
            m_lastVisibleIndices.resize(count);
            std::memcpy(m_lastVisibleIndices.data(), visibleIndices, count * sizeof(uint32_t));
            m_dirty = true;
        }

        void BatchQueue::render(rhi::IDevice* device, CommandEncoder* encoder,
            const float viewMatrix[9], const RenderWorld& world)
        {
            (void)viewMatrix; // viewMatrix 由 CommandEncoder::execute() 统一设置

            if (!encoder)
            {
                SY_ERROR("[BatchQueue] render: encoder is null, cannot submit commands");
                return;
            }

            if (m_batches.empty())
            {
                SY_DEBUGF("BatchQueue::render: m_batches is EMPTY, skipping render");
                return;
            }

            ensureIndirectCapacity(static_cast<uint32_t>(m_indirectCmds.size()));

            uint32_t totalVertices = world.getTotalVertexCount();

            if (totalVertices > m_vertexBufferCapacity)
            {
                SY_WARNF("BatchQueue::render: vertex buffer too small (%u < %u), expanding",
                    m_vertexBufferCapacity, totalVertices);
                if (m_vertexBuffer != rhi::NullHandle)
                    device->destroyBuffer(m_vertexBuffer);

                uint32_t newCap = m_vertexBufferCapacity;
                if (newCap == 0) newCap = 1024 * 1024;
                while (newCap < totalVertices) newCap *= 2;

                rhi::BufferDesc desc;
                desc.size = newCap * sizeof(VertexP3C3);
                desc.usage = rhi::BufferUsage::Vertex;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "BatchQueue_VertexBuffer";
                m_vertexBuffer = device->createBuffer(desc);
                m_vertexBufferCapacity = newCap;
            }

            device->uploadBuffer(m_vertexBuffer, 0,
                totalVertices * sizeof(VertexP3C3),
                world.getVertexData());

            if (m_dirty)
            {
                if (m_dirtyRanges.empty())
                {
                    device->uploadBuffer(m_indirectBuffer, 0,
                        m_indirectCmds.size() * sizeof(DrawIndirectCmd),
                        m_indirectCmds.data());
                }
                else
                {
                    for (const auto& range : m_dirtyRanges)
                    {
                        uint64_t offset = range.offset * sizeof(DrawIndirectCmd);
                        uint64_t size = range.size * sizeof(DrawIndirectCmd);
                        device->uploadBuffer(m_indirectBuffer, offset, size,
                            m_indirectCmds.data() + range.offset);
                    }
                }
                m_dirty = false;
            }

            // Phase 3: 通过 CommandEncoder 提交绘制命令，不再直接调用 RHI
            for (const Batch& batch : m_batches)
            {
                encoder->submitWorld(
                    batch.type,
                    batch.materialIndex,
                    batch.firstIndirect * sizeof(DrawIndirectCmd),
                    batch.indirectCount);
            }
        }

        void BatchQueue::ensureIndirectCapacity(uint32_t cmdCount)
        {
            if (cmdCount <= m_indirectBufferCapacity)
                return;

            uint32_t newCap = m_indirectBufferCapacity;
            if (newCap == 0) newCap = 512;
            while (newCap < cmdCount) newCap *= 2;

            if (m_indirectBuffer != rhi::NullHandle)
                m_device->destroyBuffer(m_indirectBuffer);

            rhi::BufferDesc desc;
            desc.size = newCap * sizeof(DrawIndirectCmd);
            desc.usage = rhi::BufferUsage::Indirect;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "BatchQueue_Indirect";

            m_indirectBuffer = m_device->createBuffer(desc);
            m_indirectBufferCapacity = newCap;
        }

        void BatchQueue::buildPipelines(rhi::IDevice* device)
        {
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

            for (uint32_t i = 0; i < PRIMITIVE_TYPE_COUNT; ++i)
            {
                rhi::PipelineDesc desc;
                desc.topology = topoMap[i];
                desc.vertexShader = kVertexShader;
                desc.fragmentShader = kFragmentShader;
                desc.computeShader = nullptr;
                desc.vertexFormat = rhi::VertexFormat::P3C3;
                desc.depthTest = false;
                desc.depthWrite = false;
                desc.blendEnable = true;
                desc.srcBlend = rhi::BlendFactor::SrcAlpha;
                desc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
                desc.depthFunc = rhi::CompareFunc::Always;

                m_pipelines[i] = device->createPipeline(desc);
            }
        }

        void BatchQueue::mergeDirtyRanges()
        {
            if (m_dirtyRanges.size() <= 1)
                return;

            std::sort(m_dirtyRanges.begin(), m_dirtyRanges.end(),
                [](const DirtyRange& a, const DirtyRange& b) {
                    return a.offset < b.offset;
                });

            std::vector<DirtyRange> merged;
            merged.push_back(m_dirtyRanges[0]);

            for (size_t i = 1; i < m_dirtyRanges.size(); ++i)
            {
                DirtyRange& last = merged.back();
                if (m_dirtyRanges[i].offset <= last.offset + last.size)
                {
                    last.size = std::max(last.size, m_dirtyRanges[i].offset + m_dirtyRanges[i].size - last.offset);
                }
                else
                {
                    merged.push_back(m_dirtyRanges[i]);
                }
            }

            m_dirtyRanges.swap(merged);
        }
    } // namespace core
} // namespace render