/**
 * @file arena.h
 * @brief 内存竞技场（Arena）分配器实现
 *
 * Arena 是一个简单高效的内存分配器，采用"一次性分配、批量释放"的策略。
 * 适合用于临时对象的分配，避免频繁的 malloc/free 调用。
 *
 * 特点：
 * - 内部使用单个连续缓冲区，分配速度极快
 * - 支持自定义对齐方式
 * - reset() 方法可以瞬间释放所有分配的内存
 * - 不支持单独释放某个对象
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <utility>
#include <vector>

/**
 * @brief 内存竞技场分配器类
 *
 * 提供高效的临时内存分配，适用于：
 * - 帧内临时对象分配
 * - 批量对象创建
 * - 避免频繁的内存分配/释放开销
 */
class Arena
{
    /// 内存缓冲区
    std::vector<uint8_t> m_buffer;
    /// 当前分配偏移量
    size_t m_offset;

    /**
     * @brief 向上对齐到指定的对齐边界
     *
     * @param value 要对齐的值
     * @param alignment 对齐边界
     * @return 对齐后的值
     */
    static size_t align_up(size_t value, size_t alignment)
    {
        return (value + alignment - 1) & ~(alignment - 1);
    }

public:
    /**
     * @brief 构造函数
     *
     * @param blockSize 初始缓冲区大小（默认64KB）
     */
    explicit Arena(size_t blockSize = 64 * 1024)
        : m_buffer(blockSize)
        , m_offset(0)
    {
    }

    /**
     * @brief 分配指定大小的内存
     *
     * @param size 要分配的大小（字节）
     * @param alignment 对齐方式（默认最大对齐）
     * @return 分配的内存指针，失败返回nullptr
     */
    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t))
    {
        size_t aligned = align_up(m_offset, alignment);
        if (aligned + size > m_buffer.size())
        {
            return nullptr;
        }
        m_offset = aligned + size;
        return m_buffer.data() + aligned;
    }

    /**
     * @brief 在竞技场中构造对象
     *
     * 使用完美转发将参数传递给对象的构造函数。
     *
     * @tparam T 对象类型
     * @tparam Args 构造参数类型
     * @param args 构造参数
     * @return 对象指针，失败返回nullptr
     */
    template<typename T, typename... Args>
    T* emplace(Args&&... args)
    {
        void* ptr = allocate(sizeof(T), alignof(T));
        if (!ptr)
        {
            return nullptr;
        }
        return new (ptr) T(std::forward<Args>(args)...);
    }

    /**
     * @brief 重置竞技场
     *
     * 将分配偏移量重置为0，相当于释放所有分配的内存。
     * 注意：不会调用对象的析构函数，适用于POD类型或临时对象。
     */
    void reset()
    {
        m_offset = 0;
    }

    /**
     * @brief 获取已使用的内存大小
     *
     * @return 已使用的字节数
     */
    size_t used() const
    {
        return m_offset;
    }

    /**
     * @brief 获取缓冲区总容量
     *
     * @return 总容量（字节）
     */
    size_t capacity() const
    {
        return m_buffer.size();
    }
};
