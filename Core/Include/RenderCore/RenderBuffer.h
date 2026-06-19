#pragma once

/**
 * @file RenderBuffer.h
 * @brief GPU缓冲区管理
 *
 * OpenGL 4.6特性使用：
 * 1. GL_APPLE_buffer_info / GL_ARB_buffer_storage - 持久映射缓冲区
 * 2. glBufferStorage - 不可变缓冲区
 * 3. GL_NV_geometry_shader_passthrough - 几何着色器优化（可选）
 */

#include "RenderCore/IRenderBackend.h"

#include <GL/glew.h>
#include <vector>
#include <cstddef>

namespace RenderCore
{

// ==================== 缓冲区类型 ====================

enum class EBufferType
{
    Vertex,      // 顶点数据
    Indirect,    // 间接绘制参数
    Staging,     // 临时上传缓冲区
};

// ==================== 内存管理策略 ====================

enum class EBufferUsage
{
    Static,      // 创建后不修改（GL_STATIC_*）
    Dynamic,      // 偶尔修改（GL_DYNAMIC_*）
    Stream,       // 每帧修改（GL_STREAM_*）
};

// ==================== GPU缓冲区 ========== =========

class GPUBuffer
{
public:
    GPUBuffer();
    ~GPUBuffer();

    // ============ 禁止拷贝 ============

    GPUBuffer(const GPUBuffer&) = delete;
    GPUBuffer& operator=(const GPUBuffer&) = delete;
    GPUBuffer(GPUBuffer&& other) noexcept;
    GPUBuffer& operator=(GPUBuffer&& other) noexcept;

    // ============ 生命周期 ============

    /**
     * @brief 创建缓冲区
     * @param type 缓冲区类型
     * @param usage 使用模式
     * @param size 缓冲区大小（字节）
     * @param data 初始数据（可选）
     */
    bool create(EBufferType type, EBufferUsage usage, size_t size, const void* data = nullptr);

    /// 销毁缓冲区
    void destroy();

    // ============ 数据上传 ============

    /**
     * @brief 上传数据到GPU
     * @param offset 缓冲区偏移
     * @param size 数据大小
     * @param data 数据指针
     *
     * 使用Buffer SubData进行增量更新
     */
    void upload(size_t offset, size_t size, const void* data);

    /**
     * @brief 完全替换缓冲区数据
     * @param size 新数据大小
     * @param data 新数据
     *
     * 使用Orphaning机制避免GPU stall
     */
    void replace(size_t size, const void* data);

    /**
     * @brief 持久映射缓冲区（OpenGL 4.4+）
     *
     * 返回直接内存指针，应用程序可直接写入
     * 使用flush/invalidate同步
     */
    void* mapPersistent(size_t offset, size_t size);
    void unmapPersistent();

    // ============ 绑定 ============

    void bind() const;
    void unbind() const;

    static void bindBase(GLenum target, GLuint bindingIndex, GLuint buffer);
    static void unbindBase(GLenum target, GLuint bindingIndex);

    // ============ 查询 ============

    GLuint getHandle() const { return m_handle; }
    size_t getSize() const { return m_size; }
    bool isValid() const { return m_handle != 0; }

    // ============ 状态重置 ============

    /// 标记整个缓冲区需要更新
    void invalidate();

    /// 丢弃旧缓冲区，创建新缓冲区（用于Orphaning）
    bool orphan(size_t newSize = 0, const void* newData = nullptr);

private:
    GLuint m_handle = 0;
    size_t m_size = 0;
    size_t m_capacity = 0;
    EBufferType m_type = EBufferType::Vertex;
    EBufferUsage m_usage = EBufferUsage::Dynamic;

    // 持久映射相关
    bool m_mapped = false;
    void* m_mappedPtr = nullptr;

    GLenum toGLTarget() const;
    GLenum toGLUsage() const;
};

// ==================== 顶点缓冲区池 ========== =========

class VertexBufferPool
{
public:
    VertexBufferPool();
    ~VertexBufferPool();

    /**
     * @brief 分配顶点缓冲区
     * @param vertexCount 顶点数
     * @param vertices 顶点数据
     * @return 缓冲区的起始偏移
     */
    size_t allocate(size_t vertexCount, const Vertex* vertices);

    /**
     * @brief 释放指定实体的缓冲区空间
     */
    void deallocate(EntityId id);

    /**
     * @brief 更新指定实体的缓冲区数据
     */
    void update(EntityId id, size_t vertexCount, const Vertex* vertices);

    /**
     * @brief 获取GPU缓冲区句柄
     */
    GLuint getBufferHandle() const { return m_buffer.getHandle(); }

    /**
     * @brief 获取顶点数据指针
     */
    const Vertex* getData() const { return m_data.data(); }

    /**
     * @brief 获取实际使用的顶点数量
     */
    size_t getUsedVertexCount() const { return m_usedVertexCount; }

    /**
     * @brief 碎片整理
     */
    void defragment(std::span<const EntityId> keepIds);

private:
    struct Allocation
    {
        size_t offset;      // 顶点偏移
        size_t count;       // 顶点数量
    };

    GPUBuffer m_buffer;
    std::vector<Vertex> m_data;          // 主机端缓存

    std::unordered_map<EntityId, Allocation> m_allocations;
    std::vector<EntityId> m_freeSlots;    // 空闲实体ID列表
    size_t m_usedVertexCount = 0;
};

} // namespace RenderCore
