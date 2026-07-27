/**
 * @file batch_queue.h
 * @brief 批量绘制队列类定义
 * 
 * BatchQueue 负责将可见实体按图元类型和材质分组，使用间接绘制（glDrawArraysIndirect）
 * 减少 CPU-GPU 通信开销。核心优化包括：
 * - 按材质和图元类型排序，减少状态切换
 * - 使用间接绘制命令缓冲区，批量提交绘制命令
 * - Dirty 范围合并，只更新修改过的区域
 */
#pragma once

#include "render/render_types.h"
#include "rhi/rhi_device.h"
#include "render_world.h"
#include <vector>
#include <cstdint>
#include <algorithm>

namespace render {
namespace core {

// Phase 3: 前向声明统一命令编码器
class CommandEncoder;

/**
 * @brief 批量绘制队列类
 * 
 * 负责：
 * - 接收可见实体列表
 * - 按图元类型和材质分组
 * - 构建间接绘制命令
 * - 执行批量绘制
 */
class BatchQueue {
public:
    /**
     * @brief 初始化批量绘制队列
     * 
     * @param device RHI设备指针
     */
    void initialize(rhi::IDevice* device);

    /**
     * @brief 关闭并释放所有资源
     */
    void shutdown();

    /**
     * @brief 提交可见实体进行批处理
     * 
     * 将可见实体按图元类型和材质分组，构建间接绘制命令。
     * 
     * @param visibleIndices 可见实体的稠密索引数组
     * @param count 可见实体数量
     * @param world 渲染世界引用
     */
    void submit(const uint32_t* visibleIndices, uint32_t count,
                const RenderWorld& world);

    /**
     * @brief 执行批量绘制
     *
     * Phase 3 起，world2D 的绘制命令不再直接调用 RHI，
     * 而是通过 CommandEncoder 统一收集和排序后执行。
     *
     * @param device     RHI设备指针
     * @param encoder    统一命令编码器（Phase 3 新增）
     * @param viewMatrix 3x3视图矩阵
     * @param world      渲染世界引用（用于获取顶点数据）
     */
    void render(rhi::IDevice* device, CommandEncoder* encoder,
                const float viewMatrix[9], const RenderWorld& world);

    /**
     * @brief 获取 world2D 顶点缓冲区句柄
     */
    rhi::BufferHandle getVertexBuffer() const { return m_vertexBuffer; }

    /**
     * @brief 获取间接命令缓冲区句柄
     */
    rhi::BufferHandle getIndirectBuffer() const { return m_indirectBuffer; }

private:
    /**
     * @brief 绘制批次结构
     * 
     * 表示一组具有相同图元类型和材质的绘制命令。
     */
    struct Batch {
        PrimitiveType type;         ///< 图元类型
        uint32_t      firstIndirect; ///< 第一个间接命令的索引
        uint32_t      indirectCount; ///< 间接命令数量
        float         lineWidth;    ///< 线宽
        uint16_t      materialIndex; ///< 材质索引
    };

    /**
     * @brief 脏范围结构
     * 
     * 表示间接命令缓冲区中需要更新的区域。
     */
    struct DirtyRange {
        uint32_t offset; ///< 偏移量（命令数量）
        uint32_t size;   ///< 大小（命令数量）
    };

    /// 间接绘制命令数组
    std::vector<DrawIndirectCmd> m_indirectCmds;
    /// 绘制批次数组
    std::vector<Batch>          m_batches;
    /// 脏范围数组（用于增量更新）
    std::vector<DirtyRange>     m_dirtyRanges;

    /// RHI设备指针
    rhi::IDevice*       m_device             = nullptr;
    /// 间接命令缓冲区
    rhi::BufferHandle   m_indirectBuffer     = rhi::NullHandle;
    /// 顶点缓冲区（存储2D实体顶点数据）
    rhi::BufferHandle   m_vertexBuffer       = rhi::NullHandle;
    /// 顶点缓冲区容量
    uint32_t            m_vertexBufferCapacity = 0;
    /// 各图元类型对应的管线
    rhi::PipelineHandle m_pipelines[7]       = {};
    /// 间接缓冲区容量
    uint32_t            m_indirectBufferCapacity = 0;
    /// 是否有脏数据需要上传
    bool                m_dirty              = false;
    /// 视图是否发生变化
    bool                m_viewChanged        = false;
    /// 上一帧的可见实体数量
    uint32_t            m_lastVisibleCount   = 0;
    /// 上一帧的可见实体索引（用于检测变化）
    std::vector<uint32_t> m_lastVisibleIndices;
    /// 上一帧的 RenderWorld 代数（用于检测顶点池重建）
    uint32_t            m_lastGeneration     = 0;

    /**
     * @brief 确保间接缓冲区容量足够
     * 
     * @param cmdCount 需要的命令数量
     */
    void ensureIndirectCapacity(uint32_t cmdCount);

    /**
     * @brief 构建各图元类型的渲染管线
     * 
     * @param device RHI设备指针
     */
    void buildPipelines(rhi::IDevice* device);

    /**
     * @brief 合并相邻的脏范围
     * 
     * 将多个相邻的脏范围合并为一个，减少上传次数。
     */
    void mergeDirtyRanges();
};

} // namespace core
} // namespace render