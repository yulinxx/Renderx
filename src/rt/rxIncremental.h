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
#include <unordered_map>
#include <vector>

namespace Render::RT::detail
{
    struct Runtime;

    // ---- 2D 分层哈希网格：空间索引 ----
    // 每层一个格子边长，层 l 的边长 = kGridBaseCellSize << l（2 的幂次）。
    // 图元按 AABB 最大边长选「能容纳它的最细层」，保证单个图元至多覆盖 2x2 个格。
    constexpr float kGridBaseCellSize = 256.0f;
    constexpr int kGridLevelCount = 12;
    constexpr uint16_t kInvalidGridLevel = 0xFFFF;

    /// 一个格子内的 slot 列表（无序，摘除用 swap-and-pop）
    struct GridCell
    {
        std::vector<uint32_t> slots;
    };

    /// 一层网格：哈希映射 (cx,cy) -> 格子
    struct GridLayer
    {
        std::unordered_map<uint64_t, GridCell> cells;
        float cellSize = 0.0f;
    };

    /**
     * @brief 按 AABB 最大边长选层：返回「层边长 >= maxExtent」的最细层
     *
     * 保证单个图元在所选层内至多跨越 2x2 个格（边长容得下，每轴至多压 2 格），
     * 这是索引插入/摘除代价可控的前提。
     *
     * 实现说明：threshold 从 2 倍基准边长起步、每层翻倍，与 m_grid[level].cellSize
     * （= kGridBaseCellSize << level）同步推进，因此 `maxExtent <= threshold` 等价于
     * `maxExtent <= cellSize(level)`。曾经它被误读成有 off-by-one，实际口径正确；
     * 边界行为由 RxRuntimeTests 的 SelectGridLevel* 用例锁定。
     */
    inline uint16_t selectGridLevel(float maxExtent)
    {
        if (maxExtent <= kGridBaseCellSize)
        {
            return 0;
        }
        float threshold = kGridBaseCellSize * 2.0f;
        for (int level = 1; level < kGridLevelCount; ++level)
        {
            if (maxExtent <= threshold)
            {
                return static_cast<uint16_t>(level);
            }
            threshold *= 2.0f;
        }
        return static_cast<uint16_t>(kGridLevelCount - 1);
    }

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
     *   1. **剔除**：用条目自带的包围盒与视口求交。2D 用世界矩形
     *      (minX,minY,maxX,maxY)，3D 用世界 AABB + 六平面视锥。包围盒存在
     *      DLL 侧，调用方不必每帧再传一遍——那份传输本身就是 O(n)。
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
        RxResult upsert3D(uint32_t slot, const DrawCommand& command, const RxAabb3* bounds);
        RxResult remove(uint32_t slot);
        RxResult clear();
        void fillStats(DrawListStats* out) const;

        /**
         * @brief 解析出本帧要绘制的命令序列（2D：世界矩形剔除）
         *
         * @param viewBounds 世界空间 (minX,minY,maxX,maxY)；nullptr 表示不剔除
         * @param culledOut  被剔除的条目数
         * @param mergedOut  合批省下的 draw 数
         * @return 内部缓存的命令数组，下一次 resolve 前保持有效
         */
        const std::vector<DrawCommand>& resolve(const float* viewBounds, uint32_t& culledOut,
                                                uint32_t& mergedOut);

        /**
         * @brief 解析出本帧要绘制的命令序列（3D：六平面视锥剔除）
         *
         * @param frustum   世界空间视锥；nullptr 表示不剔除
         * @param culledOut 被剔除的条目数
         * @param mergedOut 合批省下的 draw 数
         */
        const std::vector<DrawCommand>& resolveFrustum(const RxFrustum* frustum, uint32_t& culledOut,
                                                       uint32_t& mergedOut);

    private:
        /// 包围盒种类。0 表示「无包围盒」，该条目任何判据下都不剔除。
        enum BoundsKind : uint8_t
        {
            BoundsNone = 0,
            BoundsAabb2 = 1,
            BoundsAabb3 = 2,
        };

        struct Entry
        {
            DrawCommand command{};
            /// 包围盒数值。2D 用 [0..3]，3D 用 [0..5]，解释方式由 boundsKind 决定。
            float bounds[6]{};
            uint8_t boundsKind = BoundsNone;
            uint8_t alive = 0;
            /// 所在网格层；kInvalidGridLevel 表示未入索引（无包围盒或 3D 条目）。
            uint16_t gridLevel = kInvalidGridLevel;
            /// 帧内去重标记，防止跨格图元在查询时被重复收集。
            uint32_t frameStamp = 0;
        };

        static bool canMerge(const DrawCommand& a, const DrawCommand& b);

        /// 写入主体：两种包围盒契约只差拷贝长度与种类标记
        RxResult upsertImpl(uint32_t slot, const DrawCommand& command, const float* bounds,
                            uint8_t boundsKind);

        /// 两种判据共用的主体：排序/剔除/合批只写一遍
        const std::vector<DrawCommand>& resolveImpl(const float* viewBounds, const RxFrustum* frustum,
                                                    uint32_t& culledOut, uint32_t& mergedOut);

        /// 把一条命令追加到 m_resolved，可与末尾合并则合并（合批规则唯一实现点）
        void appendResolved(const DrawCommand& command, uint32_t& mergedOut);

        /// 线性路径：遍历有序 m_order，逐条判交 + 合批。2D/3D 判据二选一。
        void resolveLinear(bool cull2D, const float* viewBounds, bool cull3D,
                           const RxFrustum* frustum, uint32_t& culledOut, uint32_t& mergedOut);

        /// 2D 空间索引路径：网格粗筛 -> 退化回退 -> 精确判交 -> 排序 -> 合批。
        void resolveIndexed2D(const float viewBounds[4], uint32_t& culledOut,
                              uint32_t& mergedOut);

        /// 把 slot 插入 2D 网格索引（按 bounds 选层，覆盖格至多 2x2）
        void indexInsert(uint32_t slot, const float bounds[4]);

        /// 把 slot 从 2D 网格索引摘除（按记录在 Entry 里的 bounds 与层重算格子）
        void indexRemove(uint32_t slot);

        /// 清空整张网格（clear 时用，比逐条摘除快）
        void indexClear();

        /// 把非 2D 条目（无包围盒 / 3D 包围盒）加入「永可见」列表
        void indexAddNonIndexed(uint32_t slot);

        /// 从「永可见」列表摘除一个 slot（swap-and-pop）
        void indexRemoveNonIndexed(uint32_t slot);

        Runtime* m_owner = nullptr;
        /// 按 slot 直接下标。槽位由调用方分配，通常与业务图元一一对应，
        /// 因此稠密数组比哈希表更合适（查找是每帧热路径）。
        std::vector<Entry> m_entries;
        /// 存活槽位，按 sortKey 排序后的顺序
        std::vector<uint32_t> m_order;
        std::vector<DrawCommand> m_resolved;
        /// 2D 分层网格索引（每层一个 GridLayer）
        std::vector<GridLayer> m_grid;
        /// 非 2D 条目（无包围盒 / 3D 包围盒）的 slot，2D 视口下照常画、不裁
        std::vector<uint32_t> m_nonIndexed;
        /// 帧内可见候选（复用缓冲，避免每帧分配）
        std::vector<uint32_t> m_visible;
        /// 帧内去重标记（每次 resolve 递增）
        uint32_t m_stamp = 0;

        uint32_t m_entryCount = 0;
        /// 当前入 2D 网格索引的条目数（用于统计剔除数，避免遍历全部条目）
        uint32_t m_indexed2DCount = 0;
        uint32_t m_sortCount = 0;
        uint32_t m_lastVisible = 0;
        uint32_t m_lastDrawCalls = 0;
        bool m_orderDirty = true;
        bool m_enableMerging = true;
        bool m_enableCulling = true;
    };

}  // namespace Render::RT::detail
