#pragma once

/**
 * @file IRenderBackend.h
 * @brief 渲染后端抽象接口
 *
 * 设计原则：
 * 1. 面向数据(DOD)：连续内存布局，便于GPU预取和SIMD
 * 2. 显式资源管理：类似Vulkan的显式API风格
 * 3. 增量更新：支持Entity级别的增删改，无需全量上传
 * 4. 批量渲染：最大化减少绘制调用
 */

#include "RenderAPI.h"
#include "Render/RenderTypes.h"

#include <cstdint>
#include <vector>
#include <memory>
#include <functional>
#include <optional>
#include <span>

namespace RenderCore
{

// ==================== 类型前置声明 ====================

using EntityId = uint64_t;
using Generation = uint32_t;

constexpr EntityId INVALID_ENTITY_ID = 0;
constexpr size_t INVALID_INDEX = ~size_t(0);

// ==================== 图元类型 ====================

enum class EPrimitiveType : uint8_t
{
    Points         = 0,
    Lines          = 1,
    LineStrip      = 2,
    LineLoop       = 3,
    Triangles      = 4,
    TriangleStrip  = 5,
    TriangleFan    = 6,
};

// ==================== 渲染状态 ====================

struct RenderState
{
    Ut::Mat3f viewMatrix;
    float lineWidth = 1.0f;
    float pointSize = 1.0f;
    bool depthTest = false;
    bool blending = false;
};

// ==================== 顶点数据（SoA布局优化） ====================

/// 统一使用 Render::RenderVertex 作为顶点数据契约
using Vertex = Render::RenderVertex;

struct VertexBatch
{
    EPrimitiveType primitiveType = EPrimitiveType::Lines;
    std::vector<Vertex> vertices;
    float lineWidth = 1.0f;
};

// ==================== Entity渲染数据 ====================

struct EntityData
{
    EntityId id = INVALID_ENTITY_ID;
    Generation generation = 0;          // 用于检测Entity是否已修改
    std::vector<Vertex> vertices;       // 图元顶点数据
    EPrimitiveType primitiveType = EPrimitiveType::Lines;
    float lineWidth = 1.0f;
    bool deleted = false;               // 软删除标记
};

// ==================== 增量更新命令 ====================

enum class EUpdateOp : uint8_t
{
    Add,
    Modify,
    Remove,
};

struct UpdateCommand
{
    EUpdateOp op;
    EntityId entityId;
    Generation generation;
    std::vector<Vertex> vertices;        // op==Remove时忽略
    EPrimitiveType primitiveType;
    float lineWidth;
};

// ==================== 后端接口 ====================

class IRenderBackend
{
public:
    virtual ~IRenderBackend() = default;

    // ============ 生命周期 ============

    /// 初始化后端（创建GPU资源）
    virtual bool initialize() = 0;

    /// 销毁后端（释放GPU资源）
    virtual void shutdown() = 0;

    /// 开始新帧
    virtual void beginFrame() = 0;

    /// 结束当前帧
    virtual void endFrame() = 0;

    // ============ 增量更新接口 ============

    /**
     * @brief 提交增量更新命令
     * @param commands 更新命令列表
     *
     * 设计要点：
     * - Add/Modify: 只上传变化的顶点数据
     * - Remove: 标记Entity为deleted，不立即回收内存
     * - 使用Generation检测是否需要真正上传（相同generation则跳过）
     */
    virtual void submitUpdates(std::span<const UpdateCommand> commands) = 0;

    /**
     * @brief 合并已删除实体的内存碎片
     * @param keepIds 保留的EntityId列表
     *
     * 调用时机：帧间空闲时或内存紧张时
     */
    virtual void defragment(std::span<const EntityId> keepIds) = 0;

    // ============ 渲染接口 ============

    /**
     * @brief 设置渲染状态
     */
    virtual void setRenderState(const RenderState& state) = 0;

    /**
     * @brief 执行批量渲染
     * @param first 起始顶点偏移
     * @param count 顶点数量
     * @param primitiveType 图元类型
     *
     * 使用glMultiDrawArraysIndirect进行高效批量渲染
     */
    virtual void drawInstanced(size_t first, size_t count, EPrimitiveType primitiveType) = 0;

    /**
     * @brief 渲染所有已缓存的图元
     */
    virtual void renderAll() = 0;

    // ============ 状态查询 ============

    /// 获取GPU缓冲区内存使用量
    virtual size_t getBufferMemoryUsage() const = 0;

    /// 获取当前缓存的Entity数量
    virtual size_t getEntityCount() const = 0;
};

// ==================== 工厂函数（支持多后端） ====================

enum class EBackendType
{
    OpenGL45,
    OpenGL46,   // 现代OpenGL特性：GPU批次、稀疏纹理
    Vulkan,     // 预留
    DirectX12,   // 预留
};

/**
 * @brief 创建渲染后端
 * @param type 后端类型
 * @return 后端智能指针
 */
RENDER_API std::unique_ptr<IRenderBackend> createBackend(EBackendType type);

} // namespace RenderCore
