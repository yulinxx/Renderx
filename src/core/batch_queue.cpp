#include "batch_queue.h"
#include "command_encoder.h"
#include "render_world.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        bool BatchQueue::initialize(rhi::IDevice* device)
        {
            if (!device)
            {
                return false;
            }

            m_device = device;
            m_indirectCmds.reserve(512);
            m_batches.reserve(PRIMITIVE_TYPE_COUNT);
            m_dirtyRanges.reserve(64);
            // pipeline 由 CommandEncoder 统一管理，BatchQueue 不再创建

            rhi::BufferDesc vbDesc;
            vbDesc.size = 1024 * 1024 * sizeof(VertexP3C3);
            vbDesc.usage = rhi::BufferUsage::Vertex;
            vbDesc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            vbDesc.debugName = "BatchQueue_VertexBuffer";
            m_vertexBuffer = device->createBuffer(vbDesc);
            if (m_vertexBuffer == rhi::NullHandle)
            {
                SY_ERROR("[BatchQueue] failed to create vertex buffer");
                return false;
            }
            m_vertexBufferCapacity = 1024 * 1024;

            SY_DEBUG("[BatchQueue] initialized");
            return true;
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

            m_indirectBufferCapacity = 0;
            m_vertexBufferCapacity = 0;
            m_dirty = false;
            m_viewChanged = false;
            m_lastVisibleCount = 0;
            m_lastGeneration = 0;
            m_device = nullptr;
        }

        void BatchQueue::submit(const uint32_t* visibleIndices, uint32_t count, const RenderWorld& world)
        {
            // 在 submit 开始时收集顶点上传区间，避免 render 时 dirty 标志已被清除
            m_vertexUploadRanges.clear();
            RenderWorld::VertexUploadRange ranges[64];
            uint32_t rangeCount = world.getDirtyVertexRanges(ranges, 64);
            for (uint32_t i = 0; i < rangeCount; ++i)
            {
                m_vertexUploadRanges.push_back(ranges[i]);
            }

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
                // 顶点池重建后必须全量上传
                m_needFullVertexUpload = true;
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

            std::sort(sorted.begin(), sorted.end(), [](const SortEntry& a, const SortEntry& b) {
                if (a.primitiveType != b.primitiveType)
                {
                    return a.primitiveType < b.primitiveType;
                }
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

            // 全列表校验：cmd.firstVertex 与对应 entry.vertexOffset 是否一致
            // 仅在 mismatch 时通过 SY_ERRORF 输出，正常路径无日志
            bool mismatch = false;
            for (uint32_t ai = 0; ai < m_indirectCmds.size(); ++ai)
            {
                const DrawIndirectCmd& cmdm = m_indirectCmds[ai];
                const auto& e = entries[sorted[ai].visibleIdx];
                if (cmdm.vertexCount != e.vertexCount || cmdm.firstVertex != e.vertexOffset)
                {
                    mismatch = true;
                    if (ai < 16)
                    {
                        SY_ERRORF("BQ:MISMATCH cmd[%u] (first=%u,cnt=%u) vs entry (offset=%u,cnt=%u) type=%u",
                            ai,
                            cmdm.firstVertex,
                            cmdm.vertexCount,
                            e.vertexOffset,
                            e.vertexCount,
                            e.primitiveType);
                    }
                }
            }
            (void)mismatch;

            // SY_INFOF("BatchQueue::submit: REBUILD path, count=%u, entries=%zu", count, world.getEntityCount());

            m_lastVisibleCount = count;
            m_lastVisibleIndices.resize(count);
            std::memcpy(m_lastVisibleIndices.data(), visibleIndices, count * sizeof(uint32_t));
            m_dirty = true;
        }

        void BatchQueue::render(
            rhi::IDevice* device, CommandEncoder* encoder, const float viewMatrix[9], const RenderWorld& world)
        {
            (void)viewMatrix;

            if (!encoder)
            {
                SY_ERROR("[BatchQueue] render: encoder is null");
                return;
            }

            if (m_batches.empty())
            {
                return;
            }

            ensureIndirectCapacity(static_cast<uint32_t>(m_indirectCmds.size()));

            uint32_t totalVertices = world.getTotalVertexCount();

            if (totalVertices > m_vertexBufferCapacity)
            {
                SY_WARNF("BatchQueue::render: vertex buffer too small (%u < %u), expanding",
                    m_vertexBufferCapacity,
                    totalVertices);
                if (m_vertexBuffer != rhi::NullHandle)
                {
                    device->destroyBuffer(m_vertexBuffer);
                }

                uint32_t newCap = m_vertexBufferCapacity;
                if (newCap == 0)
                {
                    newCap = 1024 * 1024;
                }
                while (newCap < totalVertices)
                {
                    newCap *= 2;
                }

                rhi::BufferDesc desc;
                desc.size = newCap * sizeof(VertexP3C3);
                desc.usage = rhi::BufferUsage::Vertex;
                desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "BatchQueue_VertexBuffer";

                m_vertexBuffer = device->createBuffer(desc);
                m_vertexBufferCapacity = newCap;
                m_needFullVertexUpload = true;
            }

            // 顶点上传策略：首次渲染、扩容后或显式标记时全量上传，否则只上传 dirty 区间
            bool fullUploadPath = false;
            uint32_t uploadedVerts = 0;
            if (m_needFullVertexUpload || totalVertices > m_vertexBufferCapacity)
            {
                // 全量上传：首次渲染、扩容后或顶点池重建
                fullUploadPath = true;
                uploadedVerts = totalVertices;
                m_everFullUpload = true;
                device->uploadBuffer(m_vertexBuffer, 0, totalVertices * sizeof(VertexP3C3), world.getVertexData());
                m_needFullVertexUpload = false;
            }
            else if (!m_vertexUploadRanges.empty())
            {
                // 增量上传：只上传 submit 时收集的 dirty 区间
                for (const auto& range : m_vertexUploadRanges)
                {
                    uint64_t offset = static_cast<uint64_t>(range.vertexOffset) * sizeof(VertexP3C3);
                    uint64_t size = static_cast<uint64_t>(range.vertexCount) * sizeof(VertexP3C3);
                    uploadedVerts += range.vertexCount;
                    device->uploadBuffer(m_vertexBuffer, offset, size, world.getVertexData() + range.vertexOffset);
                }
            }

            if (m_dirty)
            {
                if (m_dirtyRanges.empty())
                {
                    device->uploadBuffer(
                        m_indirectBuffer, 0, m_indirectCmds.size() * sizeof(DrawIndirectCmd), m_indirectCmds.data());
                }
                else
                {
                    for (const auto& range : m_dirtyRanges)
                    {
                        uint64_t offset = range.offset * sizeof(DrawIndirectCmd);
                        uint64_t size = range.size * sizeof(DrawIndirectCmd);
                        device->uploadBuffer(m_indirectBuffer, offset, size, m_indirectCmds.data() + range.offset);
                    }
                }
                m_dirty = false;
            }

            // Phase 3: 通过 CommandEncoder 提交绘制命令，不再直接调用 RHI
            for (const Batch& batch : m_batches)
            {
                encoder->submitWorld(batch.type,
                    batch.materialIndex,
                    batch.firstIndirect * sizeof(DrawIndirectCmd),
                    batch.indirectCount,
                    0,  // zOrder 暂时设为 0
                    batch.lineWidth);
            }
        }

        void BatchQueue::ensureIndirectCapacity(uint32_t cmdCount)
        {
            if (cmdCount <= m_indirectBufferCapacity)
            {
                return;
            }

            uint32_t newCap = m_indirectBufferCapacity;
            if (newCap == 0)
            {
                newCap = 512;
            }
            while (newCap < cmdCount)
            {
                newCap *= 2;
            }

            if (m_indirectBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_indirectBuffer);
            }

            rhi::BufferDesc desc;
            desc.size = newCap * sizeof(DrawIndirectCmd);
            desc.usage = rhi::BufferUsage::Indirect;
            desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "BatchQueue_Indirect";

            m_indirectBuffer = m_device->createBuffer(desc);
            m_indirectBufferCapacity = newCap;
        }

        void BatchQueue::mergeDirtyRanges()
        {
            if (m_dirtyRanges.size() <= 1)
            {
                return;
            }

            std::sort(m_dirtyRanges.begin(), m_dirtyRanges.end(), [](const DirtyRange& a, const DirtyRange& b) {
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
    }  // namespace core
}  // namespace render