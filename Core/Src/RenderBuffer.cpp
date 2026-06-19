#include "RenderCore/RenderBuffer.h"

#include <GL/glew.h>
#include <cstring>
#include <algorithm>

namespace RenderCore
{

// ==================== GPUBuffer 实现 ========== =========

GPUBuffer::GPUBuffer()
{
}

GPUBuffer::~GPUBuffer()
{
    destroy();
}

GPUBuffer::GPUBuffer(GPUBuffer&& other) noexcept
    : m_handle(other.m_handle)
    , m_size(other.m_size)
    , m_capacity(other.m_capacity)
    , m_type(other.m_type)
    , m_usage(other.m_usage)
    , m_mapped(other.m_mapped)
    , m_mappedPtr(other.m_mappedPtr)
{
    other.m_handle = 0;
    other.m_size = 0;
    other.m_capacity = 0;
    other.m_mapped = false;
    other.m_mappedPtr = nullptr;
}

GPUBuffer& GPUBuffer::operator=(GPUBuffer&& other) noexcept
{
    if (this != &other)
    {
        destroy();
        m_handle = other.m_handle;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        m_type = other.m_type;
        m_usage = other.m_usage;
        m_mapped = other.m_mapped;
        m_mappedPtr = other.m_mappedPtr;

        other.m_handle = 0;
        other.m_size = 0;
        other.m_capacity = 0;
        other.m_mapped = false;
        other.m_mappedPtr = nullptr;
    }
    return *this;
}

bool GPUBuffer::create(EBufferType type, EBufferUsage usage, size_t size, const void* data)
{
    destroy();

    m_type = type;
    m_usage = usage;
    m_size = size;
    m_capacity = size;

    // 生成缓冲区对象
    glGenBuffers(1, &m_handle);
    if (m_handle == 0)
        return false;

    GLenum target = toGLTarget();
    glBindBuffer(target, m_handle);

    // 使用Buffer Storage（GL_ARB_buffer_storage）创建不可变缓冲区
    // 或者使用Buffer Data
    GLenum flags = 0;
    switch (usage)
    {
        case EBufferUsage::Static:
            flags = GL_STATIC_DRAW | GL_CLIENT_STORAGE_BIT;
            break;
        case EBufferUsage::Dynamic:
            flags = GL_DYNAMIC_DRAW;
            break;
        case EBufferUsage::Stream:
            flags = GL_STREAM_DRAW;
            break;
    }

    // 尝试使用Buffer Storage（OpenGL 4.4+）
    // 如果不支持则回退到Buffer Data
    if (GLEW_ARB_buffer_storage)
    {
        glBufferStorage(target, size, data, flags);
    }
    else
    {
        glBufferData(target, size, data, flags & ~GL_CLIENT_STORAGE_BIT);
    }

    glBindBuffer(target, 0);

    return true;
}

void GPUBuffer::destroy()
{
    if (m_mapped)
    {
        unmapPersistent();
    }

    if (m_handle)
    {
        glDeleteBuffers(1, &m_handle);
        m_handle = 0;
    }

    m_size = 0;
    m_capacity = 0;
    m_mapped = false;
    m_mappedPtr = nullptr;
}

void GPUBuffer::upload(size_t offset, size_t size, const void* data)
{
    if (!isValid() || size == 0)
        return;

    glBindBuffer(toGLTarget(), m_handle);
    glBufferSubData(toGLTarget(), offset, size, data);
    glBindBuffer(toGLTarget(), 0);

    m_size = std::max(m_size, offset + size);
}

void GPUBuffer::replace(size_t size, const void* data)
{
    if (!isValid())
        return;

    // 使用Orphaning机制避免GPU stall
    glBindBuffer(toGLTarget(), m_handle);

    // 分配新空间（自动丢弃旧数据）
    glBufferData(toGLTarget(), size, nullptr, toGLUsage());

    // 上传数据
    if (data && size > 0)
    {
        glBufferSubData(toGLTarget(), 0, size, data);
    }

    m_size = size;
    if (size > m_capacity)
    {
        m_capacity = size;
    }

    glBindBuffer(toGLTarget(), 0);
}

void* GPUBuffer::mapPersistent(size_t offset, size_t size)
{
    if (!isValid() || m_mapped)
        return nullptr;

    glBindBuffer(toGLTarget(), m_handle);

    // 使用持久映射（GL_ARB_buffer_storage需要）
    void* ptr = glMapBufferRange(toGLTarget(), offset, size,
                                  GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT);

    if (ptr)
    {
        m_mapped = true;
        m_mappedPtr = ptr;
    }

    glBindBuffer(toGLTarget(), 0);

    return ptr;
}

void GPUBuffer::unmapPersistent()
{
    if (!isValid() || !m_mapped)
        return;

    glBindBuffer(toGLTarget(), m_handle);
    glUnmapBuffer(toGLTarget());
    glBindBuffer(toGLTarget(), 0);

    m_mapped = false;
    m_mappedPtr = nullptr;
}

void GPUBuffer::bind() const
{
    glBindBuffer(toGLTarget(), m_handle);
}

void GPUBuffer::unbind() const
{
    glBindBuffer(toGLTarget(), 0);
}

void GPUBuffer::bindBase(GLenum target, GLuint bindingIndex, GLuint buffer)
{
    glBindBufferBase(target, bindingIndex, buffer);
}

void GPUBuffer::unbindBase(GLenum target, GLuint bindingIndex)
{
    glBindBufferBase(target, bindingIndex, 0);
}

void GPUBuffer::invalidate()
{
    // 标记整个缓冲区需要更新
    if (isValid())
    {
        glInvalidateBufferData(m_handle);
    }
}

bool GPUBuffer::orphan(size_t newSize, const void* newData)
{
    if (!isValid())
        return false;

    size_t sizeToUse = newSize > 0 ? newSize : m_capacity;

    glBindBuffer(toGLTarget(), m_handle);

    // 分配新空间，丢弃旧数据
    glBufferData(toGLTarget(), sizeToUse, newData, toGLUsage());

    m_size = newSize > 0 ? newSize : 0;
    m_capacity = sizeToUse;

    glBindBuffer(toGLTarget(), 0);

    return true;
}

GLenum GPUBuffer::toGLTarget() const
{
    switch (m_type)
    {
        case EBufferType::Vertex:    return GL_ARRAY_BUFFER;
        case EBufferType::Indirect:  return GL_DRAW_INDIRECT_BUFFER;
        case EBufferType::Staging:   return GL_COPY_READ_BUFFER;
        default:                      return GL_ARRAY_BUFFER;
    }
}

GLenum GPUBuffer::toGLUsage() const
{
    switch (m_usage)
    {
        case EBufferUsage::Static:   return GL_STATIC_DRAW;
        case EBufferUsage::Dynamic:  return GL_DYNAMIC_DRAW;
        case EBufferUsage::Stream:    return GL_STREAM_DRAW;
        default:                     return GL_DYNAMIC_DRAW;
    }
}

// ==================== VertexBufferPool 实现 ========== =========

VertexBufferPool::VertexBufferPool()
{
}

VertexBufferPool::~VertexBufferPool()
{
}

size_t VertexBufferPool::allocate(EntityId id, size_t vertexCount, const Vertex* vertices)
{
    // 查找空闲槽位
    for (size_t i = 0; i < m_freeSlots.size(); ++i)
    {
        EntityId freeId = m_freeSlots[i];
        auto it = m_allocations.find(freeId);
        if (it != m_allocations.end() && it->second.count >= vertexCount)
        {
            // 找到足够大的空闲槽位
            Allocation alloc = it->second;

            // 更新分配映射
            m_allocations[id] = { alloc.offset, vertexCount };

            // 如果槽位比需要的大，可能需要分裂，但简化处理直接使用整个槽位

            // 复制顶点数据
            std::copy(vertices, vertices + vertexCount,
                      m_data.begin() + alloc.offset);

            // 移除空闲槽位
            m_freeSlots.erase(m_freeSlots.begin() + i);

            // 更新已使用顶点数
            m_usedVertexCount = std::max(m_usedVertexCount,
                                          alloc.offset + vertexCount);

            return alloc.offset;
        }
    }

    // 没有合适的空闲槽位，在末尾分配
    size_t offset = m_data.size();
    m_data.resize(offset + vertexCount);
    std::copy(vertices, vertices + vertexCount, m_data.begin() + offset);

    m_allocations[id] = { offset, vertexCount };
    m_usedVertexCount = offset + vertexCount;

    // 同步到GPU
    m_buffer.replace(m_data.size() * sizeof(Vertex), m_data.data());

    return offset;
}

void VertexBufferPool::deallocate(EntityId id)
{
    auto it = m_allocations.find(id);
    if (it != m_allocations.end())
    {
        // 添加到空闲列表
        m_freeSlots.push_back(id);
        m_allocations.erase(it);
    }
}

void VertexBufferPool::update(EntityId id, size_t vertexCount, const Vertex* vertices)
{
    auto it = m_allocations.find(id);
    if (it != m_allocations.end())
    {
        Allocation& alloc = it->second;
        if (alloc.count == vertexCount)
        {
            // 大小不变，直接更新
            std::copy(vertices, vertices + vertexCount,
                      m_data.begin() + alloc.offset);

            // 增量上传
            m_buffer.upload(alloc.offset * sizeof(Vertex),
                           vertexCount * sizeof(Vertex),
                           vertices);
        }
        else
        {
            // 大小改变，重新分配
            deallocate(id);
            allocate(id, vertexCount, vertices);
        }
    }
}

void VertexBufferPool::defragment(std::span<const EntityId> keepIds)
{
    // 紧凑顶点数据
    std::vector<Vertex> compactedData;
    std::unordered_map<EntityId, Allocation> newAllocations;

    size_t currentOffset = 0;
    for (EntityId id : keepIds)
    {
        auto it = m_allocations.find(id);
        if (it != m_allocations.end())
        {
            const Allocation& alloc = it->second;

            // 复制顶点数据
            size_t oldOffset = alloc.offset;
            compactedData.insert(compactedData.end(),
                                 m_data.begin() + oldOffset,
                                 m_data.begin() + oldOffset + alloc.count);

            // 记录新偏移
            newAllocations[id] = { currentOffset, alloc.count };
            currentOffset += alloc.count;
        }
    }

    // 更新状态
    m_data = std::move(compactedData);
    m_allocations = std::move(newAllocations);
    m_usedVertexCount = m_data.size();
    m_freeSlots.clear();

    // 上传到GPU
    if (!m_data.empty())
    {
        m_buffer.replace(m_data.size() * sizeof(Vertex), m_data.data());
    }
}

} // namespace RenderCore
