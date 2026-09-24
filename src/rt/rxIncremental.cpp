/**
 * @file rxIncremental.cpp
 * @brief GeometryStore 与 DrawList 的实现（见 rxIncremental.h 的设计说明）
 */

#include "rt/rxIncremental.h"
#include "rt/rxInternal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Render::RT::detail
{
    namespace
    {
        constexpr uint64_t kDefaultInitialBytes = 4ull * 1024ull * 1024ull;
        constexpr uint32_t kDefaultGranularity = 256;
        /// 分配粒度的下限。顶点属性（float / uint32 / uchar4 归一化色）只要求
        /// 4 字节对齐，因此这是粒度真正需要满足的约束。见 initialize() 里的说明。
        constexpr uint32_t kMinGranularity = 4;
        /// 单个仓的上限：offset 是 uint32，超过 4GB 无法表达
        constexpr uint64_t kAbsoluteMaxBytes = 0xFFFFFFFFull;

        /// 一个多段批次最多容纳多少段。段表要整份交给驱动（glMultiDrawArrays
        /// 的 first/count 两个数组），几十万段一次交出去既费内存也没必要；
        /// 拆成若干批还能让驱动边提交边绘制。取 32768：70 万图元约 22 次调用，
        /// 每次的段表 256KB。
        constexpr uint32_t kMaxRangesPerBatch = 32768;

        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        /**
         * @brief 一条命令的顶点数是否落在完整的图元边界上
         *
         * 合批会把两条命令的顶点区间首尾拼成一次 draw，而列表型拓扑是按顺序
         * 成组消费顶点的：若前一段的顶点数不是完整图元数，它的「多余顶点」会
         * 与后一段的首顶点配成一条本不存在的线（LineList 奇数顶点）或一个跨越
         * 两段的三角形（TriangleList 非 3 的倍数）。这类错误只表现为多画，
         * 在密集图形里几乎看不出来，因此必须在合批前挡住。
         */
        bool isPrimitiveAligned(const PrimitiveTopology topology, uint32_t vertexCount)
        {
            switch (topology)
            {
            case PrimitiveTopology::Points:
                // 每个顶点独立，任意数量都能拼接
                return true;
            case PrimitiveTopology::Lines:
                return vertexCount % 2 == 0;
            case PrimitiveTopology::Triangles:
                return vertexCount % 3 == 0;
            default:
                // Strip / Loop 本身就不可拼接（见 canMerge）
                return false;
            }
        }

        /**
         * @brief 判断世界空间 AABB 是否完全落在视锥之外
         *
         * 逐平面分离轴：取 AABB 在该平面法线方向上**最靠内侧**的那个角，
         * 若连它都在平面外侧，整个盒子必然在外侧。反过来只要有一个角在内侧
         * 就不能排除——判据是「AABB 与视锥相交」，保守方向是多画不漏画。
         *
         * 平面约定见 renderx.h 的 RxFrustum：a*x + b*y + c*z + d >= 0 为内侧。
         */
        bool boundsOutsideFrustum(const float bounds[6], const float planes[6][4])
        {
            for (uint32_t i = 0; i < 6; ++i)
            {
                const float* plane = planes[i];
                const float x = plane[0] >= 0.0f ? bounds[3] : bounds[0];
                const float y = plane[1] >= 0.0f ? bounds[4] : bounds[1];
                const float z = plane[2] >= 0.0f ? bounds[5] : bounds[2];
                if (plane[0] * x + plane[1] * y + plane[2] * z + plane[3] < 0.0f)
                {
                    return true;
                }
            }
            return false;
        }

        /// 把两个 int32 格子坐标编码成一个 uint64 哈希键（双射，无碰撞）
        uint64_t encodeGridCell(int32_t cx, int32_t cy)
        {
            return (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32) | static_cast<uint32_t>(cy);
        }

        /// 计算 AABB 覆盖的格子坐标范围（含两端）
        void gridCellRange(
            const float bounds[4], float cellSize, int32_t& cxMin, int32_t& cxMax, int32_t& cyMin, int32_t& cyMax)
        {
            cxMin = static_cast<int32_t>(std::floor(bounds[0] / cellSize));
            cxMax = static_cast<int32_t>(std::floor(bounds[2] / cellSize));
            cyMin = static_cast<int32_t>(std::floor(bounds[1] / cellSize));
            cyMax = static_cast<int32_t>(std::floor(bounds[3] / cellSize));
        }

        /// view 是否完全包含 box。两侧布局都是 (minX, minY, maxX, maxY)。
        /// 用于判定「剔除不可能生效」，因此必须是保守方向：宁可判 false。
        bool viewContainsBox(const float view[4], const float box[4])
        {
            return view[0] <= box[0] && view[1] <= box[1] && view[2] >= box[2] && view[3] >= box[3];
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
        // 粒度只要求满足顶点属性的**实际**对齐：float / uint32 / uchar4 归一化色
        // 都是 4 字节对齐，因此 4 的倍数就够，取 16 是过度保守。
        //
        // 这里曾经强制取 16 的倍数（按 vec4 最坏情况），代价是**块大小被撑到
        // 16 的倍数**，而顶点步长通常是 24 / 28 / 36（都不是 16 的倍数），
        // 于是块与块之间必然留下空隙。DrawList 的合批要求顶点区间字节连续
        // （b.vertexOffset == a.vertexOffset + a.vertexCount * stride），
        // 有孔隙就永远合不上——实测 70 万图元 0 次合批、每帧 70 万次 draw call。
        //
        // 放开到 4 之后，调用方只要把粒度设成步长的约数（例如 P3C3 的 24 用 8），
        // 块就能紧排，合批才会真正生效。
        m_granularity = static_cast<uint32_t>(alignUp(m_granularity, kMinGranularity));

        m_forIndices = desc.forIndices != 0;

        uint64_t initial = desc.initialBytes != 0 ? desc.initialBytes : kDefaultInitialBytes;
        initial = alignUp(initial, m_granularity);

        m_maxBytes = desc.maxBytes != 0 ? (std::min)(desc.maxBytes, kAbsoluteMaxBytes) : kAbsoluteMaxBytes;
        if (initial > m_maxBytes)
        {
            m_owner->log.warn("[rt] rxGeometryStoreCreate: initialBytes(%llu) exceeds maxBytes(%llu), clamped",
                static_cast<unsigned long long>(initial),
                static_cast<unsigned long long>(m_maxBytes));  // initialBytes 超过 maxBytes，已钳制
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
        m_free.emplace(0u, static_cast<uint32_t>(initial));

        // m_owner->log.debug("[rt] Geometry store ready: capacity %llu bytes, granularity %u, %s",  // 几何仓就绪
        //     static_cast<unsigned long long>(initial),
        //     m_granularity,
        //     m_forIndices ? "indices" : "vertices");
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
            m_owner->log.error("[rt] geometry store buffer creation failed (%llu bytes)",  // 几何仓缓冲创建失败
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
                m_owner->log.error(
                    "[rt] geometry store public handle slot lost, cannot complete expansion");  // 几何仓公共句柄槽位丢失
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
            m_owner->log.error("[rt] geometry store reached limit %llu bytes, cannot allocate more",  // 几何仓已达上限
                static_cast<unsigned long long>(m_maxBytes));
            return false;
        }

        // 翻倍增长直到够用：线性增长在建大场景时会退化成 O(n²) 次搬迁。
        uint64_t next = m_capacity == 0 ? kDefaultInitialBytes : m_capacity;
        while (next < requiredCapacity)
        {
            next *= 2;
        }
        next = (std::min)(alignUp(next, m_granularity), m_maxBytes);
        if (next < requiredCapacity)
        {
            m_owner->log.error(
                "[rt] geometry store grew to limit %llu but still insufficient (need %llu)",  // 几何仓扩容到上限仍不足
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
        insertFreeRange(Range{ static_cast<uint32_t>(oldCapacity), static_cast<uint32_t>(next - oldCapacity) });

        // 新缓冲内容未定义，整段标脏，由下一次 flush 从影子重传
        m_dirty.clear();
        markDirty(0, static_cast<uint32_t>(next));

        // m_owner->log.debug("[rt] Geometry store expanded: %llu -> %llu bytes (attempt %u)",  // 几何仓扩容
        //     static_cast<unsigned long long>(oldCapacity),
        //     static_cast<unsigned long long>(next),
        //     m_growCount);

        return true;
    }

    void GeometryStore::insertFreeRange(Range range)
    {
        if (range.size == 0)
        {
            return;
        }
        // 按 offset 定位插入点并与前后相邻空洞合并，保证空闲表无相邻区间。
        // 不合并的话，反复 alloc/free 会把空闲表打成碎屑，first-fit 退化。
        // map 路径插入/摘除都是 O(log N)；旧实现用有序 vector，中间插入 O(N)
        // memmove，十万级批量释放整体 O(N^2)（实测 Debug 27.6s）。
        auto next = m_free.lower_bound(range.offset);

        // 与后邻合并
        if (next != m_free.end() && range.end() == next->first)
        {
            range.size += next->second;
            next = m_free.erase(next);
        }
        // 与前邻合并
        if (next != m_free.begin())
        {
            auto prev = next;
            --prev;
            if (prev->first + prev->second == range.offset)
            {
                prev->second += range.size;
                return;
            }
        }
        m_free.emplace(range.offset, range.size);
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
            for (auto it = m_free.begin(); it != m_free.end(); ++it)
            {
                if (it->second < need)
                {
                    continue;
                }
                const uint32_t offset = it->first;
                if (it->second == need)
                {
                    m_free.erase(it);
                }
                else
                {
                    // 从区间头部切走 need：改写 key 需重新插入（map key 不可变）
                    const uint32_t remainOffset = offset + static_cast<uint32_t>(need);
                    const uint32_t remainSize = it->second - static_cast<uint32_t>(need);
                    m_free.erase(it);
                    m_free.emplace(remainOffset, remainSize);
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
        m_owner->log.error(
            "[rt] geometry store grew but still cannot allocate %llu bytes (free table may be corrupted)",  // 几何仓扩容后仍无法分配
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

    RxResult GeometryStore::write(uint64_t blockId, uint32_t byteOffset, uint32_t sizeBytes, const void* data)
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
            m_owner->log.warn("[rt] rxGeometryWrite: block %llu invalid (possibly freed)",  // 块无效
                static_cast<unsigned long long>(blockId));

            return RxResult::ErrorInvalidHandle;
        }

        if (static_cast<uint64_t>(byteOffset) + sizeBytes > block->size)
        {
            m_owner->log.error("[rt] rxGeometryWrite: write out of bounds (block %u bytes, request %u+%u)",  // 写入越界
                block->size,
                byteOffset,
                sizeBytes);

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
            m_owner->log.warn("[rt] rxGeometryFree: block %llu already freed or never existed",  // 块已释放或从未存在
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

        std::sort(m_dirty.begin(), m_dirty.end(), [](const Range& a, const Range& b) {
            return a.offset < b.offset;
        });

        // 合并重叠与近邻区间。kDirtyMergeGap 是刻意的过度传输：
        // 多传几 KB 远比多一次 writeBuffer（含驱动侧同步）便宜。
        Range current = m_dirty[0];
        uint64_t uploaded = 0;
        RxResult result = RxResult::Ok;

        auto submit = [&](const Range& range) {
            const RHI::RhiResult wrote =
                m_owner->device->writeBuffer(m_buffer, range.offset, m_shadow.data() + range.offset, range.size);
            if (wrote != RHI::RhiResult::Ok)
            {
                m_owner->log.error("[rt] geometry store upload failed (offset=%u size=%u, %s)",
                    range.offset,  // 几何仓上传失败
                    range.size,
                    RHI::resultName(wrote));

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
                current.size = (std::max)(current.end(), next.end()) - current.offset;
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
        for (const auto& freeRange : m_free)
        {
            largest = std::max<uint64_t>(largest, freeRange.second);
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
        m_grid.resize(kGridLevelCount);
        for (int i = 0; i < kGridLevelCount; ++i)
        {
            m_grid[i].cellSize = kGridBaseCellSize * static_cast<float>(1 << i);
        }
        m_visible.reserve(desc.initialCapacity != 0 ? desc.initialCapacity : 4096);
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
        m_ranges.clear();
        m_ranges.shrink_to_fit();
        m_grid.clear();
        m_visible.clear();
        m_visible.shrink_to_fit();
        m_entryCount = 0;
        m_indexed2DCount = 0;
        m_owner = nullptr;
    }

    RxResult DrawList::upsert(uint32_t slot, const DrawCommand& command, const float* aabb)
    {
        return upsertImpl(slot, command, aabb, static_cast<uint8_t>(BoundsAabb2));
    }

    RxResult DrawList::upsert3D(uint32_t slot, const DrawCommand& command, const RxAabb3* bounds)
    {
        if (!bounds)
        {
            // nullptr 是合法输入：该条目从此不参与剔除
            return upsertImpl(slot, command, nullptr, static_cast<uint8_t>(BoundsNone));
        }
        // RxAabb3 的字段顺序与 Entry::bounds 的前六个 float 一致，按同一顺序搬
        const float values[6] = { bounds->minX, bounds->minY, bounds->minZ, bounds->maxX, bounds->maxY, bounds->maxZ };
        return upsertImpl(slot, command, values, static_cast<uint8_t>(BoundsAabb3));
    }

    RxResult DrawList::upsertImpl(uint32_t slot, const DrawCommand& command, const float* bounds, uint8_t boundsKind)
    {
        if (!m_owner)
        {
            return RxResult::ErrorInvalidHandle;
        }
        // 槽号由调用方分配（通常与业务图元一一对应），因此可能很稀疏。
        // 但稠密数组的随机访问在每帧热路径上仍胜过哈希表；
        // 只在槽号异常巨大时才拒绝，避免一次 upsert 撑爆内存。
        constexpr uint32_t kMaxSlot = 1u << 24;  // 1600 万槽 ≈ 上限
        if (slot >= kMaxSlot)
        {
            m_owner->log.error("[rt] rxDrawListUpsert: slot %u too large (limit %u). "
                               "Slots should be compactly allocated, not use the 64-bit ID of the primitive directly",
                slot,
                kMaxSlot);  // 槽号过大

            return RxResult::ErrorInvalidArgument;
        }
        
        if (slot >= m_entries.size())
        {
            m_entries.resize(slot + 1);
        }

        Entry& entry = m_entries[slot];
        const bool wasAlive = entry.alive != 0;
        const uint64_t oldSortKey = entry.command.sortKey;

        // 新包围盒类型：无包围盒视作 BoundsNone，决定条目最终落 2D 索引还是「永可见」列表。
        const uint8_t newBoundsKind = bounds ? boundsKind : static_cast<uint8_t>(BoundsNone);
        const bool wasIndexed2D = wasAlive && entry.gridLevel != kInvalidGridLevel;
        const bool willBeIndexed2D = newBoundsKind == BoundsAabb2;

        // 摘除旧位置：只改颜色/顶点范围不碰索引，与「sortKey 变了才重排」同一思路。
        bool boundsChanged = false;
        if (wasIndexed2D)
        {
            boundsChanged = !willBeIndexed2D || entry.bounds[0] != bounds[0] || entry.bounds[1] != bounds[1] ||
                entry.bounds[2] != bounds[2] || entry.bounds[3] != bounds[3];
            if (boundsChanged)
            {
                indexRemove(slot);
            }
        }
        else if (wasAlive && willBeIndexed2D)
        {
            indexRemoveNonIndexed(slot);
        }

        entry.command = command;
        if (bounds)
        {
            const uint32_t count = boundsKind == BoundsAabb3 ? 6u : 4u;
            for (uint32_t i = 0; i < count; ++i)
            {
                entry.bounds[i] = bounds[i];
            }
            entry.boundsKind = boundsKind;
        }
        else
        {
            entry.boundsKind = static_cast<uint8_t>(BoundsNone);
        }

        // 插入新位置。
        if (willBeIndexed2D)
        {
            if (!wasIndexed2D || boundsChanged)
            {
                indexInsert(slot, entry.bounds);
            }
        }
        else if (!wasAlive || wasIndexed2D)
        {
            indexAddNonIndexed(slot);
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
        if (m_entries[slot].gridLevel != kInvalidGridLevel)
        {
            indexRemove(slot);
        }
        else
        {
            indexRemoveNonIndexed(slot);
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
        indexClear();
        m_nonIndexed.clear();
        std::fill(m_entries.begin(), m_entries.end(), Entry{});
        m_order.clear();
        m_resolved.clear();
        m_ranges.clear();
        m_entryCount = 0;
        m_orderDirty = true;
        return RxResult::Ok;
    }

    bool DrawList::isIndexedDraw(const DrawCommand& command)
    {
        return command.indexCount > 0 && command.indexType != IndexType::None;
    }

    bool DrawList::canBatchSharedState(const DrawCommand& a, const DrawCommand& b)
    {
        // 拓扑必须一致：一次提交沿用的是一套管线固定状态
        if (a.topology != b.topology)
        {
            return false;
        }

        // 实例化绘制的语义不可拼接
        if (a.instanceCount > 1 || b.instanceCount > 1 || a.firstInstance != b.firstInstance)
        {
            return false;
        }

        // 任何影响管线或绑定的字段不同都不能共批
        if (a.pipelineIndex != b.pipelineIndex || a.space != b.space || a.vertexFormat != b.vertexFormat ||
            a.materialIndex != b.materialIndex || a.texture != b.texture || a.lineWidth != b.lineWidth ||
            a.pointSize != b.pointSize)
        {
            return false;
        }
        if (a.vertexBuffer != b.vertexBuffer)
        {
            return false;
        }

        // 索引绘制的索引值相对 vertexBuffer 起点是绝对值，解释方式与非索引
        // 不同，两者不能混进同一批次。
        const bool aIndexed = isIndexedDraw(a);
        const bool bIndexed = isIndexedDraw(b);
        if (aIndexed != bIndexed)
        {
            return false;
        }
        if (aIndexed)
        {
            // 索引区间也要归属同一个坐标系：vertexOffset 必须完全相同
            return a.indexBuffer == b.indexBuffer && a.indexType == b.indexType && a.vertexOffset == b.vertexOffset;
        }
        return true;
    }

    void DrawList::beginBatch(const DrawCommand& command)
    {
        ResolvedBatch batch{};
        batch.command = command;
        batch.rangeBegin = static_cast<uint32_t>(m_ranges.size());
        if (isIndexedDraw(command))
        {
            // 索引绘制不用段表：命令自带 indexOffset/indexCount，且它永远独占批次
            batch.rangeCount = 0;
        }
        else
        {
            // 批次基准偏移就是本命令的偏移，因此首段起点恒为 0
            batch.rangeCount = 1;
            m_ranges.push_back(RHI::DrawRange{ 0, command.vertexCount });
        }
        m_resolved.push_back(batch);
    }

    void DrawList::appendResolved(const DrawCommand& command, uint32_t& mergedOut)
    {
        if (!m_enableMerging || m_resolved.empty() || !canBatchSharedState(m_resolved.back().command, command))
        {
            beginBatch(command);
            return;
        }

        ResolvedBatch& tail = m_resolved.back();
        if (isIndexedDraw(command))
        {
            // 索引绘制即便状态相同也不能共批：批次只记录一套 indexOffset/
            // indexCount，两条命令的索引区间未必相接。
            beginBatch(command);
            return;
        }

        const uint32_t stride = rxVertexStride(command.vertexFormat);
        if (stride == 0 || command.vertexOffset < tail.command.vertexOffset)
        {
            beginBatch(command);
            return;
        }

        // 段起点要换算成整数顶点序号，也就是块偏移之差必须是步长的整数倍。
        // 分配粒度不能整除顶点步长时（例如粒度 16、步长 24）块之间有空隙，
        // 换算不出整数、只能另起一批 —— 这正是「粒度必须能整除步长」的由来。
        const uint32_t deltaBytes = command.vertexOffset - tail.command.vertexOffset;
        if (deltaBytes % stride != 0)
        {
            beginBatch(command);
            return;
        }

        RHI::DrawRange& last = m_ranges.back();
        const uint32_t firstVertex = deltaBytes / stride;
        // 并段：列表型拓扑、与末段字节连续、且末段边界落在完整图元上。
        // 并段后段表不增长，多段提交的数组更短，驱动侧也更省。
        //
        // LineStrip 永远走不到这里：isPrimitiveAligned 对 Strip 返回 false，
        // 因为把两条独立折线并成一段会多画一条连接线。
        const bool contiguous = firstVertex == last.firstVertex + last.vertexCount;
        if (contiguous && isPrimitiveAligned(tail.command.topology, last.vertexCount))
        {
            last.vertexCount += command.vertexCount;
        }
        else
        {
            // 段数上限：段表要整份交给驱动，几十万段一次交出去既费内存也没必要，
            // 拆成若干批反而让驱动能边提交边绘制。
            if (tail.rangeCount >= kMaxRangesPerBatch)
            {
                beginBatch(command);
                return;
            }
            m_ranges.push_back(RHI::DrawRange{ firstVertex, command.vertexCount });
            tail.rangeCount += 1;
        }

        // 累计顶点数只用于统计与日志，绘制以段表为准
        tail.command.vertexCount += command.vertexCount;
        // userData 归属变得不明确：一个批次对应多个条目。
        // 置 0 而不是保留第一个——保留会让调用方误以为能靠它反查图元。
        tail.command.userData = 0;
        mergedOut += 1;
    }

    void DrawList::resolveLinear(bool cull2D,
        const float* viewBounds,
        bool cull3D,
        const RxFrustum* frustum,
        uint32_t& culledOut,
        uint32_t& mergedOut)
    {
        // 两个判据各自独立生效：条目带的是哪一种包围盒，就用对应那一种去裁。
        // 类型不匹配时一律不裁（宁可多画）——混用两种 upsert 的列表里，
        // 用 2D 矩形去裁 3D 条目正是当初「斜视角误裁」的来源。
        for (uint32_t slot : m_order)
        {
            const Entry& entry = m_entries[slot];
            if (entry.command.vertexCount == 0 && entry.command.indexCount == 0)
            {
                continue;
            }
            if (cull2D && entry.boundsKind == BoundsAabb2)
            {
                const bool disjoint = entry.bounds[2] < viewBounds[0] || entry.bounds[0] > viewBounds[2] ||
                    entry.bounds[3] < viewBounds[1] || entry.bounds[1] > viewBounds[3];
                if (disjoint)
                {
                    culledOut += 1;
                    continue;
                }
            }
            else if (cull3D && entry.boundsKind == BoundsAabb3)
            {
                if (boundsOutsideFrustum(entry.bounds, frustum->planes))
                {
                    culledOut += 1;
                    continue;
                }
            }
            appendResolved(entry.command, mergedOut);
        }
    }

    void DrawList::resolveIndexed2D(const float viewBounds[4], uint32_t& culledOut, uint32_t& mergedOut)
    {
        // 0. 捷径：视野完全包含 2D 索引条目的包围盒并集时，剔除不可能生效。
        //    ——此时网格粗筛的候选必然覆盖全部条目，触发下面的退化回退，
        //    收集本身成了纯浪费（100 万条目实测约占帧耗时 25%）。
        //    既然结论一致，就直接把「网格收集」和「逐条判交」两段一起省掉：
        //    传 cull2D=false 表示不裁 2D 条目，而这里已经证明没有条目会被裁，
        //    因此可见集合与走完整路径完全相同。culledOut 保持 0 也是同一道理。
        //    正确性依据：并集只增不减，是真实并集的超集（见成员说明）。
        if (m_indexedBoundsValid && m_indexed2DCount > 0 && viewContainsBox(viewBounds, m_indexedBounds))
        {
            resolveLinear(false, nullptr, false, nullptr, culledOut, mergedOut);
            return;
        }

        // 1. 网格粗筛收集候选：跨格图元用 frameStamp 去重，保证每个 slot 只进一次。
        m_visible.clear();
        ++m_stamp;
        const uint32_t stamp = m_stamp;
        for (int level = 0; level < kGridLevelCount; ++level)
        {
            const float cellSize = m_grid[level].cellSize;
            int32_t cxMin, cxMax, cyMin, cyMax;
            gridCellRange(viewBounds, cellSize, cxMin, cxMax, cyMin, cyMax);
            for (int32_t cy = cyMin; cy <= cyMax; ++cy)
            {
                for (int32_t cx = cxMin; cx <= cxMax; ++cx)
                {
                    auto it = m_grid[level].cells.find(encodeGridCell(cx, cy));
                    if (it == m_grid[level].cells.end())
                    {
                        continue;
                    }
                    for (uint32_t slot : it->second.slots)
                    {
                        Entry& entry = m_entries[slot];
                        if (entry.frameStamp != stamp)
                        {
                            entry.frameStamp = stamp;
                            m_visible.push_back(slot);
                        }
                    }
                }
            }
        }

        // 网格候选数（仅 2D 索引条目，已用 frameStamp 去重）。
        // 并入非 2D 条目之前记录：后面补统计「被网格直接排除的条目数」要用它。
        const uint32_t gridCandidateCount = static_cast<uint32_t>(m_visible.size());

        // 并入非 2D 条目：无包围盒 / 3D 包围盒在 2D 视口下不裁，照常画。
        for (uint32_t slot : m_nonIndexed)
        {
            Entry& entry = m_entries[slot];
            if (entry.frameStamp != stamp)
            {
                entry.frameStamp = stamp;
                m_visible.push_back(slot);
            }
        }

        // 2. 退化回退：候选超过一半条目时，网格路径收益消失，回退线性路径。
        //    回退分支由 resolveLinear 独立统计，因此这里的补统计必须放在其后。
        if (m_visible.size() * 2 > m_entryCount)
        {
            m_visible.clear();
            resolveLinear(true, viewBounds, false, nullptr, culledOut, mergedOut);
            // 刚才已经遍历过全部条目，顺手把包围盒并集重算收紧：这一步让下一帧
            // 能命中 resolveIndexed2D 开头的捷径，从「收集完再丢弃」变成「根本不收集」。
            // 放在这里而不是每帧算，是因为只有本分支才刚付过一次全量遍历的成本。
            recomputeIndexedBounds();
            return;
        }

        // 3. 补统计被网格直接排除的 2D 条目：它们不进候选，但确实未提交给 GPU。
        //    不补这一笔，culledCommandCount 会小于线性路径，两套口径不一致。
        culledOut += m_indexed2DCount - gridCandidateCount;

        // 4. 精确判交：网格是粗筛选，cell 相交不等于 AABB 相交；
        //    非 2D 条目不判交（类型不匹配一律不裁，宁可多画）。
        uint32_t write = 0;
        for (uint32_t slot : m_visible)
        {
            const Entry& entry = m_entries[slot];
            if (entry.command.vertexCount == 0 && entry.command.indexCount == 0)
            {
                continue;
            }
            if (entry.boundsKind == BoundsAabb2)
            {
                const bool disjoint = entry.bounds[2] < viewBounds[0] || entry.bounds[0] > viewBounds[2] ||
                    entry.bounds[3] < viewBounds[1] || entry.bounds[1] > viewBounds[3];
                if (disjoint)
                {
                    culledOut += 1;
                    continue;
                }
            }
            m_visible[write++] = slot;
        }
        m_visible.resize(write);

        // 4. 按 sortKey 排序：合批要求同状态相邻。
        const std::vector<Entry>& entries = m_entries;
        std::stable_sort(m_visible.begin(), m_visible.end(), [&entries](uint32_t a, uint32_t b) {
            return entries[a].command.sortKey < entries[b].command.sortKey;
        });

        // 5. 合批。
        for (uint32_t slot : m_visible)
        {
            appendResolved(m_entries[slot].command, mergedOut);
        }
    }

    void DrawList::indexInsert(uint32_t slot, const float bounds[4])
    {
        const float extent = (std::max)(bounds[2] - bounds[0], bounds[3] - bounds[1]);
        const uint16_t level = selectGridLevel(extent);
        m_entries[slot].gridLevel = level;

        const float cellSize = m_grid[level].cellSize;
        int32_t cxMin, cxMax, cyMin, cyMax;
        gridCellRange(bounds, cellSize, cxMin, cxMax, cyMin, cyMax);
        for (int32_t cy = cyMin; cy <= cyMax; ++cy)
        {
            for (int32_t cx = cxMin; cx <= cxMax; ++cx)
            {
                m_grid[level].cells[encodeGridCell(cx, cy)].slots.push_back(slot);
            }
        }
        m_indexed2DCount += 1;

        // 维护「2D 索引条目包围盒并集」。只扩不缩，保证它始终是真实并集的超集，
        // 于是「视野 ⊇ 该并集」⇒「视野 ⊇ 真实并集」，剔除捷径不会误剔。
        if (!m_indexedBoundsValid)
        {
            m_indexedBounds[0] = bounds[0];
            m_indexedBounds[1] = bounds[1];
            m_indexedBounds[2] = bounds[2];
            m_indexedBounds[3] = bounds[3];
            m_indexedBoundsValid = true;
        }
        else
        {
            m_indexedBounds[0] = (std::min)(m_indexedBounds[0], bounds[0]);
            m_indexedBounds[1] = (std::min)(m_indexedBounds[1], bounds[1]);
            m_indexedBounds[2] = (std::max)(m_indexedBounds[2], bounds[2]);
            m_indexedBounds[3] = (std::max)(m_indexedBounds[3], bounds[3]);
        }
    }

    void DrawList::indexRemove(uint32_t slot)
    {
        Entry& entry = m_entries[slot];
        if (entry.gridLevel == kInvalidGridLevel)
        {
            return;
        }
        const uint16_t level = entry.gridLevel;
        const float cellSize = m_grid[level].cellSize;
        int32_t cxMin, cxMax, cyMin, cyMax;
        gridCellRange(entry.bounds, cellSize, cxMin, cxMax, cyMin, cyMax);
        for (int32_t cy = cyMin; cy <= cyMax; ++cy)
        {
            for (int32_t cx = cxMin; cx <= cxMax; ++cx)
            {
                auto it = m_grid[level].cells.find(encodeGridCell(cx, cy));
                if (it == m_grid[level].cells.end())
                {
                    continue;
                }
                std::vector<uint32_t>& slots = it->second.slots;
                for (size_t i = 0; i < slots.size(); ++i)
                {
                    if (slots[i] == slot)
                    {
                        slots[i] = slots.back();
                        slots.pop_back();
                        break;
                    }
                }
            }
        }
        entry.gridLevel = kInvalidGridLevel;
        m_indexed2DCount -= 1;
    }

    void DrawList::indexClear()
    {
        for (GridLayer& layer : m_grid)
        {
            layer.cells.clear();
        }
        m_indexed2DCount = 0;
        // 索引已空，包围盒并集必须一并作废；否则残留的旧并集会让剔除捷径
        // 在新一轮建表时基于过期范围做判断。
        m_indexedBoundsValid = false;
    }

    void DrawList::recomputeIndexedBounds()
    {
        bool any = false;
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;

        // 按 slot 顺序扫 m_entries（顺序访问），不扫 m_order——后者是排序序，
        // 在这里没有意义且会把顺序访问变成随机访问。
        for (const Entry& entry : m_entries)
        {
            if (entry.alive == 0 || entry.boundsKind != BoundsAabb2 || entry.gridLevel == kInvalidGridLevel)
            {
                continue;
            }
            if (!any)
            {
                minX = entry.bounds[0];
                minY = entry.bounds[1];
                maxX = entry.bounds[2];
                maxY = entry.bounds[3];
                any = true;
                continue;
            }
            minX = (std::min)(minX, entry.bounds[0]);
            minY = (std::min)(minY, entry.bounds[1]);
            maxX = (std::max)(maxX, entry.bounds[2]);
            maxY = (std::max)(maxY, entry.bounds[3]);
        }

        m_indexedBounds[0] = minX;
        m_indexedBounds[1] = minY;
        m_indexedBounds[2] = maxX;
        m_indexedBounds[3] = maxY;
        m_indexedBoundsValid = any;
    }

    void DrawList::indexAddNonIndexed(uint32_t slot)
    {
        // 调用链保证不重复入列：upsert 仅在「新建条目」或「从网格索引迁回」时调用本函数。
        // 位置记在 Entry 上，删除时才能 O(1) swap-and-pop。
        m_entries[slot].nonIndexedPos = static_cast<uint32_t>(m_nonIndexed.size());
        m_nonIndexed.push_back(slot);
    }

    void DrawList::indexRemoveNonIndexed(uint32_t slot)
    {
        Entry& entry = m_entries[slot];
        const uint32_t pos = entry.nonIndexedPos;
        if (pos == kInvalidNonIndexedPos || pos >= m_nonIndexed.size())
        {
            // 不在列表中：与旧实现一样静默返回（重复摘除是合法的防御性调用）。
            return;
        }

        // swap-and-pop：用末位 slot 补位，并把它的位置索引改到补位点。
        const uint32_t movedSlot = m_nonIndexed.back();
        m_nonIndexed[pos] = movedSlot;
        m_nonIndexed.pop_back();
        if (movedSlot != slot)
        {
            m_entries[movedSlot].nonIndexedPos = pos;
        }
        entry.nonIndexedPos = kInvalidNonIndexedPos;
    }

    const std::vector<ResolvedBatch>& DrawList::resolve(
        const float* viewBounds, uint32_t& culledOut, uint32_t& mergedOut)
    {
        return resolveImpl(viewBounds, nullptr, culledOut, mergedOut);
    }

    const std::vector<ResolvedBatch>& DrawList::resolveFrustum(
        const RxFrustum* frustum, uint32_t& culledOut, uint32_t& mergedOut)
    {
        return resolveImpl(nullptr, frustum, culledOut, mergedOut);
    }

    const std::vector<ResolvedBatch>& DrawList::resolveImpl(
        const float* viewBounds, const RxFrustum* frustum, uint32_t& culledOut, uint32_t& mergedOut)
    {
        culledOut = 0;
        mergedOut = 0;
        m_resolved.clear();
        m_ranges.clear();

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
            std::stable_sort(m_order.begin(), m_order.end(), [&entries](uint32_t lhs, uint32_t rhs) {
                return entries[lhs].command.sortKey < entries[rhs].command.sortKey;
            });
            m_orderDirty = false;
            m_sortCount += 1;
        }

        const bool cull2D = m_enableCulling && viewBounds != nullptr;
        const bool cull3D = m_enableCulling && frustum != nullptr;

        if (cull2D)
        {
            // 2D 走空间索引：网格粗筛 + 退化回退 + 精确判交 + 排序 + 合批
            resolveIndexed2D(viewBounds, culledOut, mergedOut);
        }
        else
        {
            // 3D 视锥剔除（线性）或 无剔除（全量合批）
            resolveLinear(false, nullptr, cull3D, frustum, culledOut, mergedOut);
        }

        m_lastVisible = static_cast<uint32_t>(m_resolved.size()) + mergedOut;
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