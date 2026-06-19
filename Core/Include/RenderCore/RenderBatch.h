#pragma once

/**
 * @file RenderBatch.h
 * @brief 批处理管理器
 *
 * 核心职责：
 * 1. 将相同图元类型的Entity合并为批量渲染命令
 * 2. 最小化绘制调用（Draw Call）
 * 3. 支持多类型批次的并行渲染
 *
 * 使用glMultiDrawArraysIndirect实现高效批量渲染
 */

#include "RenderCore/IRenderBackend.h"
#include "RenderCore/RenderBuffer.h"

#include <unordered_map>
#include <vector>
#include <algorithm>

namespace RenderCore
{

// ==================== Enum哈希函数 ========== =========

struct EnumHash
{
    template<typename T>
    size_t operator()(T value) const noexcept
    {
        return static_cast<size_t>(value);
    }
};

// ==================== 绘制参数结构（用于Indirect Drawing） ====================

struct DrawArraysIndirectCommand
{
    GLuint vertexCount;   // 顶点数
    GLuint instanceCount; // 实例数（目前为1）
    GLuint firstVertex;   // 起始顶点偏移
    GLuint baseInstance;  // 基础实例（用于实例化渲染）
};

// ==================== 批次渲染命令 ========== =========

struct BatchCommand
{
    EntityId entityId = INVALID_ENTITY_ID;
    EPrimitiveType primitiveType = EPrimitiveType::Lines;
    size_t firstVertex = 0;      // 在合并缓冲区中的偏移
    size_t vertexCount = 0;
    float lineWidth = 1.0f;
};

// ==================== 批次渲染器 ========== =========

class BatchRenderer
{
public:
    BatchRenderer();
    ~BatchRenderer();

    // ============ 禁止拷贝 ============

    BatchRenderer(const BatchRenderer&) = delete;
    BatchRenderer& operator=(const BatchRenderer&) = delete;

    // ============ 生命周期 ============

    bool initialize();
    void shutdown();

    // ============ 批次管理 ============

    /**
     * @brief 添加实体到批次
     * @param entityId 实体ID
     * @param vertices 顶点数据
     * @param primitiveType 图元类型
     * @param lineWidth 线宽
     *
     * 将顶点追加到对应图元类型的批次中
     */
    void addToBatch(EntityId entityId,
                    std::span<const Vertex> vertices,
                    EPrimitiveType primitiveType,
                    float lineWidth);

    /**
     * @brief 移除实体批次
     */
    void removeFromBatch(EntityId entityId);

    /**
     * @brief 更新实体批次
     */
    void updateInBatch(EntityId entityId,
                       std::span<const Vertex> vertices,
                       float lineWidth);

    /**
     * @brief 清空所有批次
     */
    void clearAllBatches();

    // ============ 渲染 ============

    /**
     * @brief 渲染所有批次
     * @param state 渲染状态
     *
     * 使用glMultiDrawArraysIndirect进行批量渲染
     */
    void render(const RenderState& state);

    // ============ 查询 ============

    size_t getBatchCount(EPrimitiveType type) const;
    size_t getTotalVertexCount() const { return m_mergedVertices.size(); }

private:
    // 合并所有批次到单个顶点缓冲区
    void mergeBatches();

    // 更新间接绘制缓冲区
    void updateIndirectBuffer();

    // 按图元类型分组渲染
    void renderByPrimitiveType(EPrimitiveType type);

private:
    // VAO
    GLuint m_vao = 0;

    // 顶点缓冲区（所有批次合并）
    GPUBuffer m_vertexBuffer;

    // 间接绘制缓冲区
    GPUBuffer m_indirectBuffer;

    // 间接绘制命令
    std::vector<DrawArraysIndirectCommand> m_indirectCommands;

    // 合并后的顶点数据
    std::vector<Vertex> m_mergedVertices;

    // 批次命令映射
    std::vector<BatchCommand> m_batchCommands;

    // 每种图元类型的批次数量
    std::unordered_map<EPrimitiveType, size_t, EnumHash> m_batchCounts;

    // 脏标记
    bool m_dirtyBatches = true;
    bool m_dirtyIndirect = true;
};

// ==================== 辅助函数 ========== =========

/// 获取图元类型的OpenGL枚举
GLenum toGLPrimitiveType(EPrimitiveType type);

/// 计算顶点数据所需的字节数
constexpr size_t calcVertexDataSize(size_t vertexCount)
{
    return vertexCount * sizeof(Vertex);
}

} // namespace RenderCore
