/**
 * @file OverlayQueue.cpp
 * @brief 叠加层渲染队列实现 —— 通用、可扩展的绘制命令队列
 */
#include "overlayQueue.h"
#include "commandEncoder.h"
#include "render/RenderTypes.h"
#include "Log/SyLogger.h"
#include <algorithm>
#include <cstring>

namespace Render
{
    namespace core
    {

        bool OverlayQueue::initialize(RHI::IDevice* device, uint32_t initialCapacity)
        {
            if (!device)
            {
                SY_ERRORF("[OverlayQueue] initialize: device is null");
                return false;
            }

            m_device = device;
            m_maxCapacity = std::max(initialCapacity, 4096u);

            // 创建 GPU 顶点缓冲区 (GPU_CPU_Coherent，支持直接映射上传)
            RHI::BufferDesc desc;
            desc.size = m_maxCapacity * sizeof(OverlayVertex);
            desc.usage = RHI::BufferUsage::Vertex;
            desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "OverlayQueue_VB";
            m_vertexBuffer = device->createBuffer(desc);

            if (m_vertexBuffer == RHI::NullHandle)
            {
                SY_ERRORF("[OverlayQueue] failed to create vertex buffer");
                return false;
            }

            m_vbCapacity = m_maxCapacity;
            m_stagingBuffer.reserve(m_maxCapacity);

            SY_DEBUGF("[OverlayQueue] initialized: capacity=%u", m_maxCapacity);
            return true;
        }

        void OverlayQueue::shutdown()
        {
            if (m_vertexBuffer != RHI::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = RHI::NullHandle;
            }

            m_stagingBuffer.clear();
            m_ranges.clear();
            m_groupIndices.clear();
            m_groupDirty.clear();
            m_vbCapacity = 0;
            m_writeOffset = 0;
            m_totalVertices = 0;
        }

        // 确保 staging buffer 有足够空间，必要时扩容
        bool OverlayQueue::ensureCapacity(uint32_t requiredVertices)
        {
            if (m_stagingBuffer.size() + requiredVertices <= m_maxCapacity)
            {
                return true;
            }

            // 尝试压缩
            compactRanges();

            if (m_stagingBuffer.size() + requiredVertices <= m_maxCapacity)
            {
                return true;
            }

            // 需要扩容 GPU 缓冲区
            uint32_t newCapacity = m_maxCapacity * 2;
            if (newCapacity < m_stagingBuffer.size() + requiredVertices)
            {
                newCapacity = m_stagingBuffer.size() + requiredVertices;
            }

            SY_DEBUGF("[OverlayQueue] resizing buffer: %u -> %u", m_maxCapacity, newCapacity);

            RHI::BufferDesc desc;
            desc.size = newCapacity * sizeof(OverlayVertex);
            desc.usage = RHI::BufferUsage::Vertex;
            desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "OverlayQueue_VB";

            RHI::BufferHandle newBuffer = m_device->createBuffer(desc);
            if (newBuffer == RHI::NullHandle)
            {
                SY_ERRORF("[OverlayQueue] failed to resize buffer");
                return false;
            }

            // 复制现有数据到新缓冲区
            if (!m_stagingBuffer.empty())
            {
                m_device->uploadBuffer(newBuffer, 0,
                    m_stagingBuffer.size() * sizeof(OverlayVertex),
                    m_stagingBuffer.data());
            }

            m_device->destroyBuffer(m_vertexBuffer);
            m_vertexBuffer = newBuffer;
            m_maxCapacity = newCapacity;
            m_vbCapacity = newCapacity;

            return true;
        }

        uint32_t OverlayQueue::submit(const OverlayVertex* vertices, uint32_t vertexCount, const DrawRange& range)
        {
            if (!vertices || vertexCount == 0 || range.vertexCount == 0)
            {
                return UINT32_MAX;
            }

            // 确保容量
            if (!ensureCapacity(vertexCount))
            {
                return UINT32_MAX;
            }

            // 写入 staging buffer
            uint32_t offset = static_cast<uint32_t>(m_stagingBuffer.size());
            m_stagingBuffer.insert(m_stagingBuffer.end(), vertices, vertices + vertexCount);

            // 记录范围
            InternalRange irange;
            irange.vertexOffset = offset;
            irange.vertexCount = vertexCount;
            irange.topology = range.topology;
            irange.group = range.group;
            irange.zOrder = range.zOrder;
            irange.isTriangle = range.isTriangle;
            irange.alive = true;

            uint32_t rangeIndex = static_cast<uint32_t>(m_ranges.size());
            m_ranges.push_back(irange);
            m_groupIndices[range.group].push_back(rangeIndex);
            m_groupDirty[range.group] = true;
            m_totalVertices += vertexCount;

            return offset;
        }

        uint32_t OverlayQueue::submitBatch(const DrawItem* items, uint32_t count)
        {
            if (!items || count == 0)
            {
                return UINT32_MAX;
            }

            // 计算总顶点数
            uint32_t totalVerts = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                totalVerts += items[i].vertexCount;
            }

            if (!ensureCapacity(totalVerts))
            {
                return UINT32_MAX;
            }

            uint32_t firstOffset = UINT32_MAX;

            for (uint32_t i = 0; i < count; ++i)
            {
                const DrawItem& item = items[i];
                if (!item.vertices || item.vertexCount == 0)
                {
                    continue;
                }

                uint32_t offset = static_cast<uint32_t>(m_stagingBuffer.size());
                if (firstOffset == UINT32_MAX)
                {
                    firstOffset = offset;
                }

                m_stagingBuffer.insert(m_stagingBuffer.end(),
                    item.vertices, item.vertices + item.vertexCount);

                InternalRange irange;
                irange.vertexOffset = offset;
                irange.vertexCount = item.vertexCount;
                irange.topology = item.range.topology;
                irange.group = item.range.group;
                irange.zOrder = item.range.zOrder;
                irange.isTriangle = item.range.isTriangle;
                irange.alive = true;

                uint32_t rangeIndex = static_cast<uint32_t>(m_ranges.size());
                m_ranges.push_back(irange);
                m_groupIndices[item.range.group].push_back(rangeIndex);
                m_groupDirty[item.range.group] = true;
            }

            return firstOffset;
        }

        void OverlayQueue::clearGroup(uint32_t group)
        {
            auto it = m_groupIndices.find(group);
            if (it == m_groupIndices.end())
            {
                return;
            }

            // 标记该 group 的所有范围为 dead
            for (uint32_t idx : it->second)
            {
                if (idx < m_ranges.size())
                {
                    m_ranges[idx].alive = false;
                }
            }

            m_groupDirty[group] = true;
        }

        void OverlayQueue::clearAll()
        {
            for (auto& range : m_ranges)
            {
                range.alive = false;
            }
            m_groupDirty.clear();
            for (const auto& pair : m_groupIndices)
            {
                m_groupDirty[pair.first] = true;
            }
        }

        void OverlayQueue::render(RHI::IDevice* device, CommandEncoder* encoder)
        {
            if (!encoder || m_ranges.empty())
            {
                return;
            }

            // 检查是否有脏数据
            bool anyDirty = false;
            for (const auto& pair : m_groupDirty)
            {
                if (pair.second)
                {
                    anyDirty = true;
                    break;
                }
            }

            if (anyDirty)
            {
                // 重建/增量更新 staging buffer
                if (anyDirty)
                {
                    rebuildStagingBuffer();
                }

                // 上传到 GPU
                uploadStagingBuffer();

                // 清除脏标记
                for (auto& pair : m_groupDirty)
                {
                    pair.second = false;
                }
            }

            // 按 zOrder 排序
            sortRangesByZOrder();

            // 提交到 CommandEncoder
            submitToEncoder(encoder);
        }

        void OverlayQueue::rebuildStagingBuffer()
        {
            // 计算存活顶点总数
            uint32_t aliveVertices = 0;
            for (const auto& range : m_ranges)
            {
                if (range.alive)
                {
                    aliveVertices += range.vertexCount;
                }
            }

            std::vector<OverlayVertex> newStaging;
            newStaging.reserve(aliveVertices);
            std::vector<InternalRange> newRanges;
            newRanges.reserve(m_ranges.size());
            std::unordered_map<uint32_t, std::vector<uint32_t>> newGroupIndices;

            uint32_t writeOffset = 0;
            for (uint32_t i = 0; i < m_ranges.size(); ++i)
            {
                const InternalRange& oldRange = m_ranges[i];
                if (!oldRange.alive)
                {
                    continue;
                }

                // 计算源偏移 (需要从旧 staging buffer 复制)
                // 这里简化：重新提交时应用层应重新提供顶点数据
                // 实际工程中，可保留旧 staging buffer 并复制
                // 此处仅演示结构，实际需应用层配合重新提交
            }

            // 实际工程中，应用层在 clearGroup 后应重新提交数据
            // 这里仅清空标记，实际顶点数据由应用层下一帧重新 submit
            for (auto& range : m_ranges)
            {
                if (!range.alive)
                {
                    range.vertexCount = 0;
                }
            }

            // 重置写入位置
            m_writeOffset = 0;
            // 注意：m_stagingBuffer 保留，等待应用层重新填充
        }

        void OverlayQueue::compactRanges()
        {
            // 移除死范围
            m_ranges.erase(
                std::remove_if(m_ranges.begin(), m_ranges.end(),
                    [](const InternalRange& r) { return !r.alive; }),
                m_ranges.end());

            // 重建 group 索引
            m_groupIndices.clear();
            for (uint32_t i = 0; i < m_ranges.size(); ++i)
            {
                m_groupIndices[m_ranges[i].group].push_back(i);
            }
        }

        void OverlayQueue::uploadStagingBuffer()
        {
            if (m_stagingBuffer.empty())
            {
                return;
            }

            uint32_t vertexCount = static_cast<uint32_t>(m_stagingBuffer.size());
            if (vertexCount > m_vbCapacity)
            {
                // 容量不足时扩容 (ensureCapacity 已处理)
                return;
            }

            m_device->uploadBuffer(m_vertexBuffer, 0,
                vertexCount * sizeof(OverlayVertex),
                m_stagingBuffer.data());
        }

        void OverlayQueue::sortRangesByZOrder()
        {
            std::stable_sort(m_ranges.begin(), m_ranges.end(),
                [](const InternalRange& a, const InternalRange& b)
                {
                    if (a.zOrder != b.zOrder)
                    {
                        return a.zOrder < b.zOrder;
                    }
                    return a.group < b.group;
                });
        }

        void OverlayQueue::submitToEncoder(CommandEncoder* encoder)
        {
            for (const auto& range : m_ranges)
            {
                if (!range.alive || range.vertexCount == 0)
                {
                    continue;
                }

                PrimitiveType topo = range.isTriangle ? PrimitiveType::TriangleList : range.topology;
                encoder->submitOverlay(topo, range.vertexOffset, range.vertexCount);
            }
        }

    }  // namespace core
}  // namespace Render