/**
 * @file persistent_entity_manager.h
 * @brief 持久实体管理器
 *
 * Phase 9 核心组件。管理稳定实体的 GPU 端持久化存储，
 * 通过 SSBO 存储实体元数据，并支持 Compute Shader 视锥剔除。
 *
 * 设计目标：
 * - 稳定实体不再每帧全量重建
 * - 视锥剔除与可见性判断更多由 GPU 承担
 * - 为大场景性能扩展提供架构出口
 */
#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>

namespace render {
namespace core {

/// 持久实体元数据（CPU 侧）
struct PersistentEntity
{
    uint32_t id;               ///< 实体唯一标识
    float    bboxMin[3];       ///< 轴对齐包围盒最小点
    float    bboxMax[3];       ///< 轴对齐包围盒最大点
    float    worldPos[3];      ///< 世界空间位置（用于距离排序等）
    uint32_t vertexOffset;     ///< 在全局顶点缓冲中的偏移
    uint32_t vertexCount;      ///< 顶点数量
    uint32_t materialIndex;    ///< 材质索引
    uint32_t flags;            ///< 标志位：bit0=可见性, bit1=静态
};

/// 持久实体 GPU 数据布局（与 SSBO 严格对齐）
struct alignas(16) EntityGpuData
{
    float bboxMin[4];          ///< xyz + padding
    float bboxMax[4];          ///< xyz + padding
    float worldPos[4];         ///< xyz + padding
    uint32_t vertexOffset;     ///< 顶点偏移
    uint32_t vertexCount;      ///< 顶点数量
    uint32_t materialIndex;    ///< 材质索引
    uint32_t flags;            ///< 标志位
};

/**
 * @brief 持久实体管理器
 *
 * 管理一组稳定实体的 GPU 端数据，提供增量更新和 GPU 剔除能力。
 *
 * 使用流程：
 *   1. initialize()                         // 初始化 SSBO
 *   2. addEntity() / removeEntity() / updateEntity()  // CPU 侧维护
 *   3. uploadChanges()                      // 增量上传到 GPU
 *   4. executeCulling(viewProjMatrix)       // GPU 视锥剔除
 *   5. generateIndirectCommands(outputBuf)  // 生成间接绘制命令
 */
class PersistentEntityManager {
public:
    PersistentEntityManager() = default;
    ~PersistentEntityManager() { shutdown(); }

    /**
     * @brief 初始化管理器
     *
     * 创建实体元数据 SSBO 和可见性结果缓冲。
     *
     * @param device RHI 设备指针
     * @param maxEntities 最大支持的实体数量（决定 SSBO 大小）
     */
    void initialize(rhi::IDevice* device, uint32_t maxEntities = 65536);

    /**
     * @brief 关闭并释放 GPU 资源
     */
    void shutdown();

    /**
     * @brief 添加实体
     *
     * @param entity 实体元数据
     * @return 实体在管理器中的索引，失败返回 UINT32_MAX
     */
    uint32_t addEntity(const PersistentEntity& entity);

    /**
     * @brief 移除实体
     *
     * 采用惰性删除策略，标记为无效，下次 uploadChanges 时同步到 GPU。
     *
     * @param index 实体索引
     */
    void removeEntity(uint32_t index);

    /**
     * @brief 清空所有实体
     *
     * 重置实体计数，保留 GPU 缓冲容量。用于每帧从 RenderWorld 全量同步前。
     */
    void clearEntities();

    /**
     * @brief 更新实体数据
     *
     * @param index 实体索引
     * @param entity 新的实体元数据
     */
    void updateEntity(uint32_t index, const PersistentEntity& entity);

    /**
     * @brief 增量上传变更到 GPU
     *
     * 只上传自上次调用以来发生变化的实体，减少 CPU-GPU 传输量。
     */
    void uploadChanges();

    /**
     * @brief 执行 GPU 视锥剔除
     *
     * 绑定 compute pipeline，dispatch compute shader 对 SSBO 中的实体
     * 进行 2D AABB-视图矩形测试，结果写入可见性缓冲。
     *
     * 剔除语义与 CPU 侧四叉树 queryVisible 保持一致：
     * 世界空间包围盒与视图矩形做 AABB 相交测试。
     *
     * @param viewMinX 视图矩形最小 X（世界空间）
     * @param viewMinY 视图矩形最小 Y（世界空间）
     * @param viewMaxX 视图矩形最大 X（世界空间）
     * @param viewMaxY 视图矩形最大 Y（世界空间）
     */
    void executeCulling(float viewMinX, float viewMinY,
                        float viewMaxX, float viewMaxY);

    /**
     * @brief 生成间接绘制命令
     *
     * 根据 GPU 剔除结果，为可见实体生成 indirect draw 命令，
     * 写入指定的 indirect buffer。
     *
     * @param outIndirectBuffer 输出 indirect buffer
     * @param outCommandCount   输出命令数量
     */
    void generateIndirectCommands(rhi::BufferHandle outIndirectBuffer,
                                  uint32_t* outCommandCount);

    /**
     * @brief 获取实体元数据 SSBO 句柄
     */
    rhi::BufferHandle getEntityBuffer() const { return m_entityBuffer; }

    /**
     * @brief 获取可见性结果缓冲句柄
     */
    rhi::BufferHandle getVisibilityBuffer() const { return m_visibilityBuffer; }

    /**
     * @brief 获取 indirect draw 缓冲句柄
     */
    rhi::BufferHandle getIndirectBuffer() const { return m_indirectBuffer; }

    /**
     * @brief 获取当前实体数量
     */
    uint32_t getEntityCount() const { return m_entityCount; }

    /**
     * @brief 获取最大实体容量
     */
    uint32_t getMaxEntities() const { return m_maxEntities; }

    /**
     * @brief 获取 compute pipeline 句柄
     */
    rhi::PipelineHandle getCullingPipeline() const { return m_cullingPipeline; }

private:
    rhi::IDevice* m_device = nullptr;
    bool m_initialized = false;

    uint32_t m_maxEntities = 0;       ///< SSBO 容量
    uint32_t m_entityCount = 0;       ///< 当前有效实体数

    /// CPU 侧实体数组（与 GPU SSBO 一一对应）
    std::vector<PersistentEntity> m_entities;
    /// 脏标记数组（true 表示对应实体需要上传）
    std::vector<bool> m_dirtyFlags;

    /// GPU 资源
    rhi::BufferHandle m_entityBuffer = rhi::NullHandle;      ///< 实体元数据 SSBO
    rhi::BufferHandle m_visibilityBuffer = rhi::NullHandle;  ///< 可见性结果缓冲
    rhi::BufferHandle m_indirectBuffer = rhi::NullHandle;    ///< indirect draw 缓冲
    rhi::BufferHandle m_countBuffer = rhi::NullHandle;       ///< 原子计数缓冲（用于写入 indirect 数量）

    /// Compute Pipeline
    rhi::PipelineHandle m_cullingPipeline = rhi::NullHandle;

    /// 统计
    uint32_t m_lastVisibleCount = 0;

    /**
     * @brief 创建或确保 compute pipeline 已就绪
     */
    void ensureCullingPipeline();
};

} // namespace core
} // namespace render
