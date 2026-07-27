/**
 * @file command_encoder.h
 * @brief 统一命令编码器类定义
 *
 * Phase 3 引入的核心组件，负责将 overlay 和 world2D 的绘制命令
 * 收集到同一条内部链路，统一排序后执行。
 *
 * 设计目标：
 * - CPU 侧只负责描述收集
 * - GPU 侧按 batch key 分组，减少状态切换和 draw call
 * - 为后续 MDI / batch 合并 / render graph 预留架构出口
 */
#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>
#include <functional>

namespace render {
namespace core {

/// 绘制空间类型，用于区分数据来源
enum class DrawSpace : uint8_t
{
    World2D  = 0,  ///< 2D 文档几何空间（来自 BatchQueue / RenderWorld）
    Overlay  = 1,  ///< 叠加层空间（来自 OverlayQueue）
};

/// 统一的 batch key，64bit，按优先级从高到低编码
/// 排序规则（数值小者优先）：space → z-order → topology → material
using BatchKey = uint64_t;

/**
 * @brief 统一的绘制命令描述
 *
 * 收集来自 overlay 和 world 的所有绘制意图，
 * 由 CommandEncoder 统一排序后批量执行。
 */
struct DrawCommand
{
    BatchKey      sortKey;        ///< 排序键，决定绘制顺序
    DrawSpace     space;          ///< 绘制空间
    PrimitiveType topology;       ///< 图元拓扑类型
    uint16_t      materialIndex;  ///< 材质索引（World2D 有效）
    uint32_t      zOrder;         ///< Z 序（Overlay 有效）

    // 绘制参数联合体
    union {
        // World2D 路径：使用间接绘制
        struct {
            uint32_t indirectOffset;  ///< 间接命令在 buffer 中的字节偏移
            uint32_t indirectCount;   ///< 间接命令数量
        } world;

        // Overlay 路径：使用直接绘制
        struct {
            uint32_t vertexOffset;  ///< 顶点在 overlay VB 中的偏移
            uint32_t vertexCount;   ///< 顶点数量
        } overlay;
    };
};

/**
 * @brief 统一命令编码器
 *
 * Phase 3 核心组件，统一收集和排序 overlay / world 绘制命令。
 *
 * 使用流程（每帧）：
 *   1. reset()                    // 清空上一帧命令
 *   2. submitOverlay(...) x N     // OverlayQueue 提交
 *   3. submitWorld(...) x N       // BatchQueue 提交
 *   4. execute(...)               // 统一排序并执行
 */
class CommandEncoder {
public:
    CommandEncoder();
    ~CommandEncoder();

    /**
     * @brief 初始化编码器
     *
     * 创建内部 pipeline 缓存。
     *
     * @param device RHI 设备指针
     */
    void initialize(rhi::IDevice* device);

    /**
     * @brief 关闭并释放资源
     */
    void shutdown();

    /**
     * @brief 重置命令列表
     *
     * 每帧开始时调用，清空上一帧的所有命令。
     */
    void reset();

    /**
     * @brief 提交 overlay 绘制命令
     *
     * 由 OverlayQueue::render() 调用，把 overlay 的绘制意图注册到编码器。
     *
     * @param topology     图元拓扑
     * @param vertexOffset overlay 顶点 buffer 中的偏移
     * @param vertexCount  顶点数量
     * @param zOrder       Z 序（默认 100，确保 overlay 在 world 之上）
     */
    void submitOverlay(PrimitiveType topology,
                       uint32_t vertexOffset, uint32_t vertexCount,
                       uint32_t zOrder = 100);

    /**
     * @brief 提交 world2D 绘制命令
     *
     * 由 BatchQueue::render() 调用，把每个 batch 的绘制意图注册到编码器。
     *
     * @param topology      图元拓扑
     * @param materialIndex 材质索引
     * @param indirectOffset 间接命令在 indirect buffer 中的字节偏移
     * @param indirectCount  间接命令数量
     * @param zOrder         Z 序（默认 0）
     */
    void submitWorld(PrimitiveType topology, uint16_t materialIndex,
                     uint32_t indirectOffset, uint32_t indirectCount,
                     uint32_t zOrder = 0);

    /**
     * @brief 执行所有收集的命令
     *
     * 按 sortKey 排序后，统一绑定 pipeline / buffer 并执行绘制。
     * 相同 topology + material + space 的命令会被连续执行，减少状态切换。
     *
     * @param device      RHI 设备
     * @param worldVB     world2D 顶点 buffer（来自 BatchQueue）
     * @param overlayVB   overlay 顶点 buffer（来自 OverlayQueue）
     * @param indirectBuf 间接命令 buffer（来自 BatchQueue）
     * @param viewMatrix  3x3 视图矩阵
     */
    void execute(rhi::IDevice* device,
                 rhi::BufferHandle worldVB,
                 rhi::BufferHandle overlayVB,
                 rhi::BufferHandle indirectBuf,
                 const float viewMatrix[9]);

    /**
     * @brief 获取当前命令数量
     */
    uint32_t getCommandCount() const;

    /**
     * @brief 获取上一帧的批次数量（按 sortKey 分组后）
     */
    uint32_t getBatchCount() const { return m_lastBatchCount; }

private:
    std::vector<DrawCommand> m_commands;
    rhi::IDevice* m_device = nullptr;
    bool m_initialized = false;

    // pipeline 缓存
    rhi::PipelineHandle m_overlayLinePipeline = {};    ///< overlay 线段管线
    rhi::PipelineHandle m_overlayTriPipeline  = {};    ///< overlay 三角形管线
    rhi::PipelineHandle m_worldPipelines[PRIMITIVE_TYPE_COUNT] = {}; ///< world 管线

    // 统计
    uint32_t m_lastBatchCount = 0;

    /**
     * @brief 构建排序键
     *
     * 64bit 编码规则（从高到低优先级）：
     *   bits 0-7:   space (0=World2D, 1=Overlay)
     *   bits 8-23:  z-order (uint16, 小值先画)
     *   bits 24-31: topology (PrimitiveType)
     *   bits 32-47: material index
     *   bits 48-63: 保留
     */
    static BatchKey buildSortKey(DrawSpace space, uint32_t zOrder,
                                  PrimitiveType topology, uint16_t materialIndex);

    /**
     * @brief 获取 overlay 对应的 pipeline
     */
    rhi::PipelineHandle getOverlayPipeline(PrimitiveType topology) const;

    /**
     * @brief 获取 world 对应的 pipeline
     */
    rhi::PipelineHandle getWorldPipeline(PrimitiveType topology) const;

    /**
     * @brief 绑定并执行单个命令
     */
    void dispatchCommand(rhi::IDevice* device, const DrawCommand& cmd,
                         rhi::BufferHandle worldVB,
                         rhi::BufferHandle overlayVB,
                         rhi::BufferHandle indirectBuf,
                         const float viewMatrix[9]);
};

} // namespace core
} // namespace render
