/**
 * @file rxIncremental.cpp
 * @brief GeometryStore 与 DrawList 的实现（见 rxIncremental.h 的设计说明）
 */

#include "rt/rxIncremental.h"
#include "rt/rxInternal.h"

#include <algorithm>
#include <cstring>

namespace Render::RT::detail
{

    namespace
    {
        constexpr uint64_t kDefaultInitialBytes = 4ull * 1024ull * 1024ull;
        constexpr uint32_t kDefaultGranularity = 256;
        /// 单个仓的上限：offset 是 uint32，超过 4GB 无法表达
        constexpr uint64_t kAbsoluteMaxBytes = 0xFFFFFFFFull;

        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }
    }  // namespace

    // ======================================================================
    // GeometryStore
    // ======================================================================

    bool GeometryStore::initialize(Runtime* owner, const GeometryStoreDesc& desc)
    {
        m_owner = owner;
        if (!m_owner || !m_owner->device)
        {
            return false;
        }

        m_granularity = desc.granularity != 0 ? desc.granularity : kDefaultGranularity;
        // 顶点属性最坏对齐是 16 字节（vec4），粒度必须是它的倍数，
        // 否则块起始偏移可能不满足属性对齐要求，某些驱动上会直接画错。
        m_granularity = static_cast<uint32_t>(alignUp(m_granularity, 16));

        m_forIndices = desc.forIndices != 0;

        uint64_t initial = desc.initialBytes != 0 ? desc.initialBytes : kDefaultInitialBytes;
        initial = alignUp(initial, m_granularity);

        m_maxBytes = desc.maxBytes != 0 ? std::min(desc.maxBytes, kAbsoluteMaxBytes) : kAbsoluteMaxBytes;
        if (initial > m_maxBytes)
        {
            m_owner->log.warn("[rt] rxGeometryStoreCreate: initialBytes(%llu) 超过 maxBytes(%llu)，已钳制",
                              static_cast<unsigned long long>(initial),
                              static_cast<unsigned long long>(m_maxBytes));
            initial = m_maxBytes;
        }

        if (!createBuffer(initial))
        {
            return false;
        }

        m_shadow.assign(static_cast<size_t>(initial), 0);
        m_capacity = initial;
        m_used = 0;
        m_free.clear();
        m_free.push_back(Range{ 0, static_cast<uint32_t>(initial) });

        m_owner->log.debug("[rt] Geometry store ready: capacity %llu bytes, granularity %u, %s",  // 几何仓就绪
                          static_cast<unsigned long long>(initial), m_granularity,
                          m_forIndices ? "indices" : "vertices");
        return true;
    }

    bool GeometryStore::createBuffer(uint64_t capacity)
    {
        RHI::BufferDesc desc{};
        desc.size = capacity;
        // 同时标 Vertex|Index：CAD 场景里同一个仓常常两种数据混放，
        // 分成两个仓只会让碎片翻倍。
        desc.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Index;
        // CpuToGpu 而非 GpuOnly：增量写是常态，需要可写路径。
        desc.access = RHI::MemoryAccess::CpuToGpu;
        desc.debugName = m_forIndices ? "RxGeometryStoreIndex" : "RxGeometryStoreVertex";

        const RHI::BufferHandle created = m_owner->device->createBuffer(desc);
        if (!created.valid())
        {
            m_owner->log.error("[rt] 几何仓缓冲创建失败（%llu 字节）",
                               static_cast<unsigned long long>(capacity));
            return false;
        }

        // 替换而不是保留旧缓冲：旧内容已在 CPU 影子里，搬迁由 grow() 负责。
        if (m_buffer.valid())
        {
            m_owner->device->destroyBuffer(m_buffer);
            // 原地改写同一个槽位的值，这样调用方手里的 BufferHandle 数值不变
            // 也能指向新缓冲。若改用 erase+insert，世代会自增，
            // 已发出的所有 GeometryBlock::buffer 会集体失效。
            RHI::BufferHandle* slot = m_owner->buffers.find(static_cast<uint64_t>(m_publicBuffer));
            if (!slot)
            {
                // 槽位应当一直存在（shutdown 才会摘除），走到这里说明句柄表被外部改坏了
                m_owner->log.error("[rt] 几何仓公共句柄槽位丢失，无法完成扩容");
                m_owner->device->destroyBuffer(created);
                return false;
            }
            *slot = created;
        }
        else
        {
            m_publicBuffer = static_cast<BufferHandle>(m_owner->buffers.insert(created));
        }
        m_buffer = created;
        return true;
    }

    void GeometryStore::shutdown()
    {
        if (!m_owner)
        {
            return;
        }
        if (m_publicBuffer != BufferHandle::Invalid)
        {
            // 从公共句柄表摘除，避免调用方拿旧句柄命中已销毁的缓冲
            m_owner->buffers.erase(static_cast<uint64_t>(m_publicBuffer));
            m_publicBuffer = BufferHandle::Invalid;
        }
        if (m_buffer.valid())
        {
            m_owner->device->destroyBuffer(m_buffer);
            m_buffer = RHI::BufferHandle{};
        }
        m_shadow.clear();
        m_shadow.shrink_to_fit();
        m_blocks.clear();
        m_free.clear();
        m_dirty.clear();
        m_capacity = 0;
        m_used = 0;
        m_owner = nullptr;
    }

    bool GeometryStore::grow(uint64_t requiredCapacity)
    {
        if (m_capacity >= m_maxBytes)
        {
            m_owner->log.error("[rt] 几何仓已达上限 %llu 字节，无法继续分配",
                               static_cast<unsigned long long>(m_maxBytes));
            return false;
        }

        // 翻倍增长直到够用：线性增长在建大场景时会退化成 O(n²) 次搬迁。
        uint64_t next = m_capacity == 0 ? kDefaultInitialBytes : m_capacity;
        while (next < requiredCapacity)
        {
            next *= 2;
        }
        next = std::min(alignUp(next, m_granularity), m_maxBytes);
        if (next < requiredCapacity)
        {
            m_owner->log.error("[rt] 几何仓扩容到上限 %llu 仍不足（需要 %llu）",
                               static_cast<unsigned long long>(m_maxBytes),
                               static_cast<unsigned long long>(requiredCapacity));
            return false;
        }

        const uint64_t oldCapacity = m_capacity;
        if (!createBuffer(next))
        {
            return false;
        }

        m_shadow.resize(static_cast<size_t>(next), 0);
        m_capacity = next;
        m_growCount += 1;

        // 尾部新增空间进空闲表；已有块的偏移不变，所以只需要补这一段
        insertFreeRange(Range{ static_cast<uint32_t>(oldCapacity),
                               static_cast<uint32_t>(next - oldCapacity) });

        // 新缓冲内容未定义，整段标脏，由下一次 flush 从影子重传
        m_dirty.clear();
        markDirty(0, static_cast<uint32_t>(next));

        m_owner->log.debug("[rt] Geometry store expanded: %llu -> %llu bytes (attempt %u)",  // 几何仓扩容
                          static_cast<unsigned long long>(oldCapacity),
                          static_cast<unsigned long long>(next), m_growCount);
        return true;
    }

    void GeometryStore::insertFreeRange(Range range)
    {
        if (range.size == 0)
        {
            return;
        }
        // 按 offset 找插入位；随后与前后合并，保证空闲表里不存在相邻空洞。
        // 不合并的话，反复 alloc/free 会把空闲表打成碎屑，first-fit 退化。
        auto pos = std::lower_bound(m_free.begin(), m_free.end(), range,
                                    [](const Range& a, const Range& b) { return a.offset < b.offset; });
        auto inserted = m_free.insert(pos, range);

        if (inserted + 1 != m_free.end() && inserted->end() == (inserted + 1)->offset)
        {
            inserted->size += (inserted + 1)->size;
            m_free.erase(inserted + 1);
        }
        if (inserted != m_free.begin())
        {
            auto prev = inserted - 1;
            if (prev->end() == inserted->offset)
            {
                prev->size += inserted->size;
                m_free.erase(inserted);
            }
        }
    }

    RxResult GeometryStore::allocate(uint64_t sizeBytes, GeometryBlock* out)
    {
        if (!out || sizeBytes == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }

        const uint64_t need = alignUp(sizeBytes, m_granularity);
        if (need > 0xFFFFFFFFull)
        {
            return RxResult::ErrorInvalidArgument;
        }

        bool grown = false;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            // first-fit：空闲表已按 offset 有序且无相邻空洞，first-fit 的
            // 定位性比 best-fit 好——同类图元倾向落在相邻位置，
            // 这正是 DrawList 合批能生效的前提。
            for (size_t i = 0; i < m_free.size(); ++i)
            {
                if (m_free[i].size < need)
                {
                    continue;
                }
                const uint32_t offset = m_free[i].offset;
                if (m_free[i].size == need)
                {
                    m_free.erase(m_free.begin() + static_cast<ptrdiff_t>(i));
                }
                else
                {
                    m_free[i].offset += static_cast<uint32_t>(need);
                    m_free[i].size -= static_cast<uint32_t>(need);
                }

                Block block{};
                block.offset = offset;
                block.size = static_cast<uint32_t>(need);
                const uint64_t id = m_blocks.insert(block);
                m_used += need;

                out->buffer = m_publicBuffer;
                out->id = id;
                out->offset = offset;
                out->sizeBytes = static_cast<uint32_t>(sizeBytes);
                return grown ? RxResult::ErrorGeometryStoreGrown : RxResult::Ok;
            }

            if (attempt == 0)
            {
                if (!grow(m_capacity + need))
                {
                    return RxResult::ErrorOutOfMemory;
                }
                grown = true;
            }
        }

        // 扩容成功却仍找不到空洞 = 空闲表与容量不一致，属于内部错误
        m_owner->log.error("[rt] 几何仓扩容后仍无法分配 %llu 字节（空闲表可能已损坏）",
                           static_cast<unsigned long long>(need));
        return RxResult::ErrorOutOfMemory;
    }

    void GeometryStore::markDirty(uint32_t offset, uint32_t size)
    {
        if (size == 0)
        {
            return;
        }
        m_dirty.push_back(Range{ offset, size });
    }

    RxResult GeometryStore::write(uint64_t blockId, uint32_t byteOffset, uint32_t sizeBytes,
                                 const void* data)
    {
        if (!data || sizeBytes == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }

        const Block* block = m_blocks.find(blockId);
        if (!block)
        {
            m_owner->log.warn("[rt] rxGeometryWrite: 块 %llu 无效（可能已释放）",
                              static_cast<unsigned long long>(blockId));
            return RxResult::ErrorInvalidHandle;
        }
        if (static_cast<uint64_t>(byteOffset) + sizeBytes > block->size)
        {
            m_owner->log.error("[rt] rxGeometryWrite: 写入越界（块 %u 字节，请求 %u+%u）", block->size,
                               byteOffset, sizeBytes);
            return RxResult::ErrorInvalidArgument;
        }

        const uint32_t absolute = block->offset + byteOffset;
        std::memcpy(m_shadow.data() + absolute, data, sizeBytes);
        markDirty(absolute, sizeBytes);
        return RxResult::Ok;
    }

    RxResult GeometryStore::release(uint64_t blockId)
    {
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }
        const Block* block = m_blocks.find(blockId);
        if (!block)
        {
            m_owner->log.warn("[rt] rxGeometryFree: 块 %llu 已释放或从未存在",
                              static_cast<unsigned long long>(blockId));
            return RxResult::ErrorInvalidHandle;
        }

        const Range freed{ block->offset, block->size };
        m_used -= block->size;
        m_blocks.erase(blockId);
        insertFreeRange(freed);
        // 释放不标脏：残留字节不会被任何 draw 引用（对应的 DrawCommand
        // 也应该同时从 DrawList 移除）。为「保险」而重传一遍已释放区域
        // 是纯粹的浪费。
        return RxResult::Ok;
    }

    RxResult GeometryStore::flush()
    {
        if (!m_owner || !m_owner->device)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (m_dirty.empty())
        {
            return RxResult::Ok;
        }

        std::sort(m_dirty.begin(), m_dirty.end(),
                  [](const Range& a, const Range& b) { return a.offset < b.offset; });

        // 合并重叠与近邻区间。kDirtyMergeGap 是刻意的过度传输：
        // 多传几 KB 远比多一次 writeBuffer（含驱动侧同步）便宜。
        Range current = m_dirty[0];
        uint64_t uploaded = 0;
        RxResult result = RxResult::Ok;

        auto submit = [&](const Range& range) {
            const RHI::RhiResult wrote = m_owner->device->writeBuffer(
                m_buffer, range.offset, m_shadow.data() + range.offset, range.size);
            if (wrote != RHI::RhiResult::Ok)
            {
                m_owner->log.error("[rt] 几何仓上传失败（offset=%u size=%u，%s）", range.offset,
                                   range.size, RHI::resultName(wrote));
                result = RxResult::ErrorDeviceLost;
                return;
            }
            uploaded += range.size;
        };

        for (size_t i = 1; i < m_dirty.size(); ++i)
        {
            const Range& next = m_dirty[i];
            if (next.offset <= current.end() + kDirtyMergeGap)
            {
                current.size = std::max(current.end(), next.end()) - current.offset;
            }
            else
            {
                submit(current);
                current = next;
            }
        }
        submit(current);

        m_dirty.clear();
        m_uploadBytesThisFrame += uploaded;
        return result;
    }

    void GeometryStore::fillStats(GeometryStoreStats* out) const
    {
        if (!out)
        {
            return;
        }
        out->capacityBytes = m_capacity;
        out->usedBytes = m_used;
        out->blockCount = static_cast<uint32_t>(m_blocks.size());
        out->freeRangeCount = static_cast<uint32_t>(m_free.size());
        out->growCount = m_growCount;
        out->_pad0 = 0;

        uint64_t largest = 0;
        for (const Range& range : m_free)
        {
            largest = std::max<uint64_t>(largest, range.size);
        }
        out->largestFreeBytes = largest;

        uint64_t dirty = 0;
        for (const Range& range : m_dirty)
        {
            dirty += range.size;
        }
        out->dirtyBytesThisFrame = dirty;
    }

    // ======================================================================
    // DrawList
    // ======================================================================

    bool DrawList::initialize(Runtime* owner, const DrawListDesc& desc)
    {
        m_owner = owner;
        if (!m_owner)
        {
            return false;
        }
        m_enableMerging = desc.enableMerging != 0;
        m_enableCulling = desc.enableCulling != 0;
        if (desc.initialCapacity != 0)
        {
            m_entries.resize(desc.initialCapacity);
            m_order.reserve(desc.initialCapacity);
            m_resolved.reserve(desc.initialCapacity);
        }
        return true;
    }

    void DrawList::shutdown()
    {
        m_entries.clear();
        m_entries.shrink_to_fit();
        m_order.clear();
        m_order.shrink_to_fit();
        m_resolved.clear();
        m_resolved.shrink_to_fit();
        m_entryCount = 0;
        m_owner = nullptr;
    }

    RxResult DrawList::upsert(uint32_t slot, const DrawCommand& command, const float* aabb)
    {
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }
        // 槽号由调用方分配（通常与业务实体一一对应），因此可能很稀疏。
        // 但稠密数组的随机访问在每帧热路径上仍胜过哈希表；
        // 只在槽号异常巨大时才拒绝，避免一次 upsert 撑爆内存。
        constexpr uint32_t kMaxSlot = 1u << 24;  // 1600 万槽 ≈ 上限
        if (slot >= kMaxSlot)
        {
            m_owner->log.error("[rt] rxDrawListUpsert: 槽号 %u 过大（上限 %u）。"
                               "槽号应紧凑分配，不要直接用实体的 64 位 ID",
                               slot, kMaxSlot);
            return RxResult::ErrorInvalidArgument;
        }
        if (slot >= m_entries.size())
        {
            m_entries.resize(slot + 1);
        }

        Entry& entry = m_entries[slot];
        const bool wasAlive = entry.alive != 0;
        const uint64_t oldSortKey = entry.command.sortKey;

        entry.command = command;
        if (aabb)
        {
            std::memcpy(entry.aabb, aabb, sizeof(entry.aabb));
            entry.hasAabb = 1;
        }
        else
        {
            entry.hasAabb = 0;
        }

        if (!wasAlive)
        {
            entry.alive = 1;
            m_entryCount += 1;
            m_orderDirty = true;
        }
        else if (oldSortKey != command.sortKey)
        {
            // 只有排序键变了才需要重排。仅改顶点范围/颜色不触发排序——
            // 这是「每帧只 upsert 变化条目」能真正省下 CPU 的关键。
            m_orderDirty = true;
        }
        return RxResult::Ok;
    }

    RxResult DrawList::remove(uint32_t slot)
    {
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }
        if (slot >= m_entries.size() || m_entries[slot].alive == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        m_entries[slot] = Entry{};
        m_entryCount -= 1;
        m_orderDirty = true;
        return RxResult::Ok;
    }

    RxResult DrawList::clear()
    {
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }
        // 保留容量：清空后通常紧接着重建同量级的场景
        std::fill(m_entries.begin(), m_entries.end(), Entry{});
        m_order.clear();
        m_resolved.clear();
        m_entryCount = 0;
        m_orderDirty = true;
        return RxResult::Ok;
    }

    bool DrawList::canMerge(const DrawCommand& a, const DrawCommand& b)
    {
        // 只有列表型拓扑可以拼接。Strip/Loop 合并会把两条独立折线连起来，
        // 多画一段——而且这种错误在密集图形里几乎看不出来。
        switch (a.topology)
        {
        case PrimitiveTopology::Points:
        case PrimitiveTopology::Lines:
        case PrimitiveTopology::Triangles:
            break;
        default:
            return false;
        }
        if (a.topology != b.topology)
        {
            return false;
        }

        // 实例化绘制的语义不可拼接
        if (a.instanceCount > 1 || b.instanceCount > 1 || a.firstInstance != b.firstInstance)
        {
            return false;
        }

        // 任何影响管线或绑定的字段不同都不能合并
        if (a.pipelineIndex != b.pipelineIndex || a.space != b.space ||
            a.vertexFormat != b.vertexFormat || a.materialIndex != b.materialIndex ||
            a.texture != b.texture || a.lineWidth != b.lineWidth || a.pointSize != b.pointSize)
        {
            return false;
        }
        if (a.vertexBuffer != b.vertexBuffer)
        {
            return false;
        }

        const bool aIndexed = a.indexCount > 0 && a.indexType != IndexType::None;
        const bool bIndexed = b.indexCount > 0 && b.indexType != IndexType::None;
        if (aIndexed != bIndexed)
        {
            return false;
        }

        if (aIndexed)
        {
            // 索引绘制：索引值是相对 vertexBuffer 起点的绝对值，
            // 因此还要求两者的 vertexOffset 完全相同，否则索引解释会错位。
            if (a.indexBuffer != b.indexBuffer || a.indexType != b.indexType ||
                a.vertexOffset != b.vertexOffset)
            {
                return false;
            }
            const uint32_t indexStride = a.indexType == IndexType::Uint32 ? 4u : 2u;
            return b.indexOffset == a.indexOffset + a.indexCount * indexStride;
        }

        // 非索引绘制：顶点必须字节连续
        const uint32_t stride = rxVertexStride(a.vertexFormat);
        if (stride == 0)
        {
            return false;
        }
        return b.vertexOffset == a.vertexOffset + a.vertexCount * stride;
    }

    const std::vector<DrawCommand>& DrawList::resolve(const float* viewBounds, uint32_t& culledOut,
                                                      uint32_t& mergedOut)
    {
        culledOut = 0;
        mergedOut = 0;
        m_resolved.clear();

        if (m_orderDirty)
        {
            m_order.clear();
            for (uint32_t slot = 0; slot < m_entries.size(); ++slot)
            {
                if (m_entries[slot].alive != 0)
                {
                    m_order.push_back(slot);
                }
            }
            const std::vector<Entry>& entries = m_entries;
            // 稳定排序：同 sortKey 的条目维持槽位顺序，叠放才可预测
            std::stable_sort(m_order.begin(), m_order.end(),
                             [&entries](uint32_t lhs, uint32_t rhs) {
                                 return entries[lhs].command.sortKey < entries[rhs].command.sortKey;
                             });
            m_orderDirty = false;
            m_sortCount += 1;
        }

        const bool cull = m_enableCulling && viewBounds != nullptr;
        uint32_t visible = 0;

        for (uint32_t slot : m_order)
        {
            const Entry& entry = m_entries[slot];
            if (entry.command.vertexCount == 0 && entry.command.indexCount == 0)
            {
                continue;
            }
            if (cull && entry.hasAabb != 0)
            {
                const bool disjoint = entry.aabb[2] < viewBounds[0] || entry.aabb[0] > viewBounds[2] ||
                                      entry.aabb[3] < viewBounds[1] || entry.aabb[1] > viewBounds[3];
                if (disjoint)
                {
                    culledOut += 1;
                    continue;
                }
            }
            visible += 1;

            if (m_enableMerging && !m_resolved.empty() && canMerge(m_resolved.back(), entry.command))
            {
                DrawCommand& target = m_resolved.back();
                target.vertexCount += entry.command.vertexCount;
                target.indexCount += entry.command.indexCount;
                // userData 归属变得不明确：合并后的 draw 对应多个条目。
                // 置 0 而不是保留第一个——保留会让调用方误以为能靠它反查实体。
                target.userData = 0;
                mergedOut += 1;
                continue;
            }
            m_resolved.push_back(entry.command);
        }

        m_lastVisible = visible;
        m_lastDrawCalls = static_cast<uint32_t>(m_resolved.size());
        return m_resolved;
    }

    void DrawList::fillStats(DrawListStats* out) const
    {
        if (!out)
        {
            return;
        }
        out->entryCount = m_entryCount;
        out->visibleCount = m_lastVisible;
        out->drawCallCount = m_lastDrawCalls;
        out->sortCount = m_sortCount;
        out->capacityBytes = static_cast<uint64_t>(m_entries.capacity()) * sizeof(Entry) +
                             static_cast<uint64_t>(m_order.capacity()) * sizeof(uint32_t) +
                             static_cast<uint64_t>(m_resolved.capacity()) * sizeof(DrawCommand);
    }

}  // namespace Render::RT::detail
