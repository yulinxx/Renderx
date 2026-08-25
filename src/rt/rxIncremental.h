/**
 * @file rxIncremental.h
 * @brief 增量渲染的两块基础设施：持久几何仓与保留式绘制列表
 *
 * ## 为什么需要它们
 *
 * 瞬态环（`TransientRing`）解决的是「每帧都变」的数据。它的语义是每帧全量重传，
 * 对预览线、橡皮筋、覆盖层是对的，对常驻场景是灾难：
 * 10 万条线段改动一条，也要把 10 万条重新搬一遍。
 *
 * CAD 的负载特征恰恰相反 —— **绝大多数图元帧间完全不变**。所以需要两件东西：
 *
 * - `GeometryStore`：顶点常驻显存，编辑只重写变化的那一块。
 *   免掉「重传顶点」的开销。
 * - `DrawList`：DrawCommand 由 DLL 持有，调用方只 upsert 变化的槽位。
 *   免掉「重建命令」的开销 —— 在 10 万条的量级上，这部分 CPU 开销
 *   与重传顶点是同一个量级，只解决其中一个没有意义。
 *
 * 两者配合还带来第三个收益：同类图元的块通常在仓内相邻分配，于是
 * 「状态相同 + 顶点区间连续」成为常态，`DrawList` 可以把它们合并成
 * 极少数 draw call。
 */
#pragma once

#include "core/slotMap.h"
#include "render/renderx.h"
#include "rhi/rhiGpuDevice.h"
#include "rhi/rhiLog.h"

#include <cstdint>
#include <vector>

namespace Render::RT::detail
{
    struct Runtime;

    // ======================================================================
    // 持久几何仓
    // ======================================================================

    /**
     * @brief 可增量更新的顶点/索引存储
     *
     * ## 结构
     *
     * 一个 GPU 缓冲 + 一份等大的 CPU 影子 + 块表 + 空闲表 + 脏区表。
     *
     * CPU 影子不是可选的冗余：
     * 1. 部分写（只改块内某几个字节）之后要能把整个脏区间一次性提交，
     *    必须有一份完整的字节视图；
     * 2. 扩容时要把旧内容搬到新缓冲，从 GPU 回读远比从内存拷贝慢；
     * 3. Null 后端与不支持持久映射的驱动上行为一致。
     * 代价是 1 倍显存大小的内存占用，对 CAD 场景（几十 MB 量级）可接受。
     *
     * ## 脏区合并
     *
     * `write()` 只登记脏区间，不上传。`flush()` 时排序并合并相邻区间
     * （间隙小于 kDirtyMergeGap 也合并——多传几百字节远比多一次
     * writeBuffer 调用便宜）。「改 1 万个小块」因此不会变成 1 万次传输。
     */
    class GeometryStore
    {
    public:
        bool initialize(Runtime* owner, const GeometryStoreDesc& desc);
        void shutdown();

        RxResult allocate(uint64_t sizeBytes, GeometryBlock* out);
        RxResult write(uint64_t blockId, uint32_t byteOffset, uint32_t sizeBytes, const void* data);
        RxResult release(uint64_t blockId);
        RxResult flush();

        void fillStats(GeometryStoreStats* out) const;

        /// 公共 BufferHandle（已登记进 Runtime 的句柄表）
        BufferHandle publicBuffer() const { return m_publicBuffer; }
        /// 本帧已上传字节数，帧末由 Session 读取后清零
        uint64_t uploadBytesThisFrame() const { return m_uploadBytesThisFrame; }
        void resetFrameCounters() { m_uploadBytesThisFrame = 0; }
        bool hasPendingDirty() const { return !m_dirty.empty(); }

    private:
        struct Block
        {
            uint32_t offset = 0;
            uint32_t size = 0;
        };

        struct Range
        {
            uint32_t offset = 0;
            uint32_t size = 0;
            uint32_t end() const { return offset + size; }
        };

        /// 相邻脏区间的合并间隙上限：多传这点字节比多一次 writeBuffer 便宜
        static constexpr uint32_t kDirtyMergeGap = 4096;

        bool grow(uint64_t requiredCapacity);
        bool createBuffer(uint64_t capacity);
        void insertFreeRange(Range range);
        void markDirty(uint32_t offset, uint32_t size);

        Runtime* m_owner = nullptr;
        RHI::BufferHandle m_buffer{};
        BufferHandle m_publicBuffer = BufferHandle::Invalid;

        std::vector<uint8_t> m_shadow;
        SlotMap<uint64_t, Block> m_blocks;
        /// 按 offset 升序，且保证互不相邻（相邻的已合并）
        std::vector<Range> m_free;
        std::vector<Range> m_dirty;

        uint64_t m_capacity = 0;
        uint64_t m_maxBytes = 0;
        uint64_t m_used = 0;
        uint32_t m_granularity = 256;
        uint32_t m_growCount = 0;
        uint64_t m_uploadBytesThisFrame = 0;
        bool m_forIndices = false;
    };

    // ======================================================================
    // 保留式绘制列表
    // ======================================================================

    /**
     * @brief DLL 侧持有的 DrawCommand 集合
     *
     * 调用方按槽位 upsert，只在图元真正变化时调用。每帧提交时 DLL 做：
     *
     *   1. **剔除**：用条目自带的 AABB 与视口矩形求交。AABB 存在 DLL 侧，
     *      调用方不必每帧再传一遍——那份传输本身就是 O(n)。
     *   2. **排序**：只在有 upsert/remove 后重排，不是每帧。
     *      稳定排序保证同 sortKey 的条目维持插入顺序。
     *   3. **合批**：相邻条目状态相同且顶点区间连续时合成一次 draw。
     *
     * 合批的安全边界（写在这里是因为搞错会静默画错）：
     * - 只有 **列表型拓扑**（Points / Lines / Triangles）可以合并。
     *   Strip / Loop 合并会把两条独立折线连起来，多画一段。
     * - 必须 instanceCount == 1：实例化绘制的语义不可拼接。
     * - 顶点必须字节连续：`b.vertexOffset == a.vertexOffset + a.vertexCount * stride`。
     */
    class DrawList
    {
    public:
        bool initialize(Runtime* owner, const DrawListDesc& desc);
        void shutdown();

        RxResult upsert(uint32_t slot, const DrawCommand& command, const float* aabb);
        RxResult remove(uint32_t slot);
        RxResult clear();
        void fillStats(DrawListStats* out) const;

        /**
         * @brief 解析出本帧要绘制的命令序列
         *
         * @param viewBounds 世界空间 (minX,minY,maxX,maxY)；nullptr 表示不剔除
         * @param culledOut  被剔除的条目数
         * @param mergedOut  合批省下的 draw 数
         * @return 内部缓存的命令数组，下一次 resolve 前保持有效
         */
        const std::vector<DrawCommand>& resolve(const float* viewBounds, uint32_t& culledOut,
                                                uint32_t& mergedOut);

    private:
        struct Entry
        {
            DrawCommand command{};
            float aabb[4]{};
            uint8_t hasAabb = 0;
            uint8_t alive = 0;
        };

        static bool canMerge(const DrawCommand& a, const DrawCommand& b);

        Runtime* m_owner = nullptr;
        /// 按 slot 直接下标。槽位由调用方分配，通常与业务实体一一对应，
        /// 因此稠密数组比哈希表更合适（查找是每帧热路径）。
        std::vector<Entry> m_entries;
        /// 存活槽位，按 sortKey 排序后的顺序
        std::vector<uint32_t> m_order;
        std::vector<DrawCommand> m_resolved;

        uint32_t m_entryCount = 0;
        uint32_t m_sortCount = 0;
        uint32_t m_lastVisible = 0;
        uint32_t m_lastDrawCalls = 0;
        bool m_orderDirty = true;
        bool m_enableMerging = true;
        bool m_enableCulling = true;
    };

}  // namespace Render::RT::detail
