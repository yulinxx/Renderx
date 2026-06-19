#pragma once

/**
 * @file GL46Backend.h
 * @brief OpenGL 4.6渲染后端实现
 *
 * 特性使用：
 * 1. GL_ARB_buffer_storage - 持久映射缓冲区，避免CPU-GPU同步
 * 2. GL_ARB_direct_state_access - 直接访问缓冲区，无需绑定
 * 3. GL_ARB_multi_draw_indirect - 批量绘制调用
 * 4. GL_ARB_vertex_attrib_binding - 解耦VAO和VBO
 * 5. GL_ARB_pipeline_statistics_query - 渲染统计
 *
 * 架构设计：
 * - 渲染器为纯数据消费者，GPU缓冲区由RenderWorld管理
 * - 使用批量渲染优化减少Draw Call
 * - 增量更新通过Generation检测跳过无变化数据
 */

#include "RenderCore/IRenderBackend.h"
#include "RenderCore/RenderBatch.h"
#include "RenderCore/RenderBuffer.h"

#include <unordered_map>
#include <vector>
#include <memory>

namespace RenderCore
{

// ==================== 后端内部状态 ========== =========

struct BackendState
{
    RenderState renderState;

    // GPU缓冲区
    GPUBuffer vertexBuffer;          // 顶点数据
    GPUBuffer indirectBuffer;        // 间接绘制参数

    // VAO
    GLuint vao = 0;

    // 着色器程序
    GLuint sceneProgram = 0;

    // Uniform位置缓存
    GLint locViewMatrix = -1;
    GLint locUniformColor = -1;
    GLint locUseVertexColor = -1;

    // 内存统计
    size_t vertexBufferMemory = 0;
    size_t indirectBufferMemory = 0;
};

// ==================== OpenGL 4.6后端 ========== =========

class GL46Backend : public IRenderBackend
{
public:
    GL46Backend();
    ~GL46Backend() override;

    // ============ IRenderBackend 实现 ============

    bool initialize() override;
    void shutdown() override;

    void beginFrame() override;
    void endFrame() override;

    void submitUpdates(std::span<const UpdateCommand> commands) override;
    void defragment(std::span<const EntityId> keepIds) override;

    void setRenderState(const RenderState& state) override;

    void drawInstanced(size_t first, size_t count, EPrimitiveType primitiveType) override;
    void renderAll() override;

    size_t getBufferMemoryUsage() const override;
    size_t getEntityCount() const override { return m_entityCount; }

private:
    // 内部方法
    bool compileShaders();
    bool createVertexArray();
    bool createIndirectBuffer();

    void ensureVertexCapacity(size_t requiredVertices);
    void uploadEntityData(const UpdateCommand& cmd);

    // 顶点布局：
    // 0: position (vec3, offset 0)
    // 1: color (vec3, offset 12)

    static constexpr size_t VERTEX_STRIDE = sizeof(Vertex);
    static constexpr size_t POSITION_OFFSET = 0;
    static constexpr size_t COLOR_OFFSET = sizeof(Render::Vec3f);

    // 初始缓冲区大小
    static constexpr size_t INITIAL_VERTEX_CAPACITY = 1024 * 1024;  // 100万顶点
    static constexpr size_t INITIAL_INDIRECT_CAPACITY = 1024;        // 1000个批次

    // 扩容因子
    static constexpr float GROWTH_FACTOR = 1.5f;

private:
    // 内部状态
    BackendState m_state;

    // 顶点数据缓存
    std::vector<Vertex> m_vertexCache;

    // Entity数据映射（用于增量更新）
    struct EntityInfo
    {
        size_t bufferOffset;  // 在顶点缓冲区中的偏移
        size_t vertexCount;
        Generation generation;
    };
    std::unordered_map<EntityId, EntityInfo> m_entityMap;

    // 脏实体列表（待上传）
    std::vector<EntityId> m_dirtyEntities;

    // 当前帧的更新命令
    std::vector<UpdateCommand> m_pendingCommands;

    // 批次渲染器
    BatchRenderer m_batchRenderer;

    // 统计
    size_t m_entityCount = 0;
    size_t m_frameVertexCount = 0;

    // 初始化标记
    bool m_initialized = false;
};

// ==================== 工厂函数实现 ========== =========

inline std::unique_ptr<IRenderBackend> createBackend(EBackendType type)
{
    switch (type)
    {
        case EBackendType::OpenGL46:
            return std::make_unique<GL46Backend>();
        case EBackendType::OpenGL45:
            // TODO: 实现OpenGL 4.5后端（不支持持久映射）
            return nullptr;
        case EBackendType::Vulkan:
            // TODO: Vulkan后端
            return nullptr;
        default:
            return nullptr;
    }
}

} // namespace RenderCore
