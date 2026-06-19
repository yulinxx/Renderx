#pragma once

/**
 * @file RenderWorld.h
 * @brief 渲染世界管理器
 *
 * 核心职责：
 * 1. 管理所有Entity的渲染数据
 * 2. 维护EntityId到数据的映射
 * 3. 追踪脏数据，产生增量更新命令
 * 4. 支持多窗口共享同一World
 */

#include "RenderCore/IRenderBackend.h"
#include "RenderCore/RenderBuffer.h"
#include "Render/RenderTypes.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

namespace RenderCore
{

// ==================== 脏标记类型 ====================

enum class EDirtyFlag : uint8_t
{
    None      = 0,
    Transform = 1 << 0,   // 变换矩阵变化
    Vertices  = 1 << 1,   // 顶点数据变化
    Color     = 1 << 2,   // 颜色变化
    All       = Transform | Vertices | Color,
};

constexpr EDirtyFlag operator|(EDirtyFlag a, EDirtyFlag b)
{
    return EDirtyFlag(uint8_t(a) | uint8_t(b));
}

constexpr EDirtyFlag operator&(EDirtyFlag a, EDirtyFlag b)
{
    return EDirtyFlag(uint8_t(a) & uint8_t(b));
}

constexpr bool hasFlag(EDirtyFlag flags, EDirtyFlag flag)
{
    return (flags & flag) != EDirtyFlag::None;
}

// ==================== Entity渲染条目 ====================

struct EntityEntry
{
    EntityId id = INVALID_ENTITY_ID;
    Generation generation = 0;
    EDirtyFlag dirtyFlags = EDirtyFlag::All;  // 新增实体默认全脏

    // 渲染数据缓存
    std::vector<Vertex> vertices;
    EPrimitiveType primitiveType = EPrimitiveType::Lines;
    float lineWidth = 1.0f;

    // 空间数据（用于视锥裁剪）
    Render::BBox3f boundingBox;

    // 子系统数据指针（可选，由使用者管理）
    void* userData = nullptr;
};

// ==================== 渲染世界 ========== =========

class RenderWorld
{
public:
    RenderWorld();
    ~RenderWorld();

    // ============ 禁止拷贝 ============

    RenderWorld(const RenderWorld&) = delete;
    RenderWorld& operator=(const RenderWorld&) = delete;
    RenderWorld(RenderWorld&&) = default;
    RenderWorld& operator=(RenderWorld&&) = default;

    // ============ 生命周期 ============

    /// 初始化渲染世界
    bool initialize(EBackendType backendType = EBackendType::OpenGL46);

    /// 销毁渲染世界
    void shutdown();

    /// 每帧调用，产生增量更新
    void update();

    // ============ Entity管理 ============

    /**
     * @brief 添加或更新实体
     * @param id 实体唯一ID
     * @param vertices 顶点数据
     * @param primitiveType 图元类型
     * @param lineWidth 线宽
     *
     * 如果Entity已存在则更新，否则添加
     */
    void setEntity(EntityId id,
                   std::vector<Vertex> vertices,
                   EPrimitiveType primitiveType = EPrimitiveType::Lines,
                   float lineWidth = 1.0f);

    /**
     * @brief 批量添加或更新实体
     */
    void setEntities(std::span<const EntityId> ids,
                     std::span<const std::vector<Vertex>> vertices,
                     std::span<const EPrimitiveType> primitiveTypes,
                     std::span<const float> lineWidths);

    /**
     * @brief 删除实体
     */
    void removeEntity(EntityId id);

    /**
     * @brief 批量删除实体
     */
    void removeEntities(std::span<const EntityId> ids);

    /// 清空所有实体
    void clear();

    // ============ 渲染 ============

    /**
     * @brief 设置渲染状态并执行渲染
     */
    void render(const RenderState& state);

    /**
     * @brief 获取后端
     */
    IRenderBackend* getBackend() const { return m_backend.get(); }

    // ============ 查询 ============

    size_t getEntityCount() const { return m_entities.size(); }
    size_t getDirtyCount() const;

    /// 检查实体是否存在
    bool hasEntity(EntityId id) const;

    /// 获取实体数据（只读）
    const EntityEntry* getEntity(EntityId id) const;

    /// 获取已删除但未回收的实体数量
    size_t getPendingDeleteCount() const { return m_pendingDelete.size(); }

    // ============ 回调 ============

    /// 设置World更新回调（每帧调用，可用于同步Engine数据）
    using UpdateCallback = std::function<void()>;
    void setUpdateCallback(UpdateCallback cb) { m_updateCallback = std::move(cb); }

private:
    // 内部方法
    void markDirty(EntityId id, EDirtyFlag flags);
    void processDirtyEntities();
    void commitUpdates();

private:
    // 渲染后端
    std::unique_ptr<IRenderBackend> m_backend;

    // Entity存储（EntityId -> Entry）
    std::unordered_map<EntityId, EntityEntry> m_entities;

    // 脏实体集合（用于高效遍历）
    std::vector<EntityId> m_dirtyEntities;

    // 待删除实体（软删除）
    std::unordered_set<EntityId> m_pendingDelete;

    // 当前帧的更新命令
    std::vector<UpdateCommand> m_updateCommands;

    // 帧号（用于调试）
    uint64_t m_frameCount = 0;

    // 更新回调
    UpdateCallback m_updateCallback;

    // 同步锁（多窗口安全）
    // std::mutex m_mutex;  // TODO: 如果多线程需要
};

// ==================== 便捷类型别名 ========== =========

using RenderWorldPtr = std::unique_ptr<RenderWorld>;

} // namespace RenderCore
