/**
 * @file slot_map.h
 * @brief 插槽映射（SlotMap）数据结构实现
 *
 * SlotMap 是一种高效的键值映射数据结构，支持：
 * - O(1) 的插入、删除和查找操作
 * - 键的世代（generation）机制，防止悬垂指针
 * - 稠密数组存储值，保证内存连续性
 * - 稀疏数组映射键到稠密索引
 *
 * 键的格式：
 * - 低32位：稀疏数组索引
 * - 高32位：世代计数器
 *
 * 应用场景：
 * - 图元组件系统（ECS）中的图元存储
 * - 需要频繁插入和删除的动态集合
 * - 需要稳定迭代顺序的场景
 */
#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

/**
 * @brief 插槽映射模板类
 *
 * @tparam Key 键类型（必须是64位）
 * @tparam Value 值类型
 */
template<typename Key, typename Value>
class SlotMap
{
    /// 静态断言：Key 必须是64位类型
    static_assert(sizeof(Key) == sizeof(uint64_t), "Key must be 64-bit");

    /**
     * @brief 稀疏条目结构
     *
     * 存储稀疏索引到稠密索引的映射关系，包含世代计数器。
     */
    struct SparseEntry
    {
        uint32_t dense_index;  ///< 对应的稠密数组索引
        uint32_t generation;   ///< 世代计数器（防止悬垂指针）
    };

    /// 稠密数组，存储实际值（保证内存连续）
    std::vector<Value> m_dense;
    /// 稠密数组中每个元素对应的稀疏索引
    std::vector<uint32_t> m_dense_keys;
    /// 稀疏数组，从稀疏索引映射到稠密索引
    std::vector<SparseEntry> m_sparse;
    /// 空闲稀疏索引链表
    std::vector<uint32_t> m_freeList;

    /**
     * @brief 从键中提取稀疏索引（低32位）
     *
     * @param k 键
     * @return 稀疏索引
     */
    static uint32_t sparse_index(Key k)
    {
        return static_cast<uint32_t>(k & 0xFFFFFFFF);
    }

    /**
     * @brief 从键中提取世代计数器（高32位）
     *
     * @param k 键
     * @return 世代计数器
     */
    static uint32_t generation_from_key(Key k)
    {
        return static_cast<uint32_t>(k >> 32);
    }

    /**
     * @brief 创建键（组合世代和索引）
     *
     * @param generation 世代计数器
     * @param index 稀疏索引
     * @return 组合后的键
     */
    static Key make_key(uint32_t generation, uint32_t index)
    {
        return static_cast<Key>(static_cast<uint64_t>(generation) << 32) | static_cast<Key>(index);
    }

    /**
     * @brief 分配一个稀疏槽位，返回其索引
     *
     * 世代计数器从 1 起算，因此 make_key 永远不会产生 0。
     * 这是必要的：全代码库以 0 作为无效句柄（RHI::NullHandle、
     * RTBufferHandle 等），而旧实现的第一个槽位 generation=0、index=0，
     * 组合出的 key 恰好是 0，与「无效」不可区分。
     */
    uint32_t allocate_slot()
    {
        const uint32_t dense_pos = static_cast<uint32_t>(m_dense.size());
        if (!m_freeList.empty())
        {
            const uint32_t sparse_idx = m_freeList.back();
            m_freeList.pop_back();
            bump_generation(m_sparse[sparse_idx].generation);
            m_sparse[sparse_idx].dense_index = dense_pos;
            return sparse_idx;
        }
        const uint32_t sparse_idx = static_cast<uint32_t>(m_sparse.size());
        m_sparse.push_back({ dense_pos, kFirstGeneration });
        return sparse_idx;
    }

    /// 世代自增，并跳过 0（32 位回绕时才会发生）
    static void bump_generation(uint32_t& generation)
    {
        generation += 1;
        if (generation == 0)
        {
            generation = kFirstGeneration;
        }
    }

    static constexpr uint32_t kFirstGeneration = 1;

public:
    /**
     * @brief 插入值（拷贝版本）
     *
     * @param v 要插入的值
     * @return 新分配的键
     */
    Key insert(const Value& v)
    {
        const uint32_t sparse_idx = allocate_slot();
        m_dense.push_back(v);
        m_dense_keys.push_back(sparse_idx);
        return make_key(m_sparse[sparse_idx].generation, sparse_idx);
    }

    /**
     * @brief 插入值（移动版本）
     *
     * @param v 要插入的值（右值引用）
     * @return 新分配的键，保证非 0
     */
    Key insert(Value&& v)
    {
        const uint32_t sparse_idx = allocate_slot();
        m_dense.push_back(std::move(v));
        m_dense_keys.push_back(sparse_idx);
        return make_key(m_sparse[sparse_idx].generation, sparse_idx);
    }

    /**
     * @brief 删除键对应的值
     *
     * 删除后，键的世代会增加，旧的键将失效。
     *
     * @param k 要删除的键
     */
    void erase(Key k)
    {
        uint32_t si = sparse_index(k);
        assert(si < m_sparse.size());
        SparseEntry& se = m_sparse[si];
        assert(se.generation == generation_from_key(k));

        uint32_t di = se.dense_index;
        uint32_t last = static_cast<uint32_t>(m_dense.size() - 1);

        if (di != last)
        {
            m_dense[di] = std::move(m_dense[last]);
            m_dense_keys[di] = m_dense_keys[last];
            m_sparse[m_dense_keys[di]].dense_index = di;
        }

        m_dense.pop_back();
        m_dense_keys.pop_back();
        bump_generation(se.generation);
        se.dense_index = UINT32_MAX;
        m_freeList.push_back(si);
    }

    /**
     * @brief 查找键对应的值（非const版本）
     *
     * @param k 键
     * @return 值的指针，未找到返回nullptr
     */
    Value* find(Key k)
    {
        uint32_t si = sparse_index(k);
        if (si >= m_sparse.size())
        {
            return nullptr;
        }
        const SparseEntry& se = m_sparse[si];
        if (se.generation != generation_from_key(k))
        {
            return nullptr;
        }
        if (se.dense_index >= m_dense.size())
        {
            return nullptr;
        }
        return &m_dense[se.dense_index];
    }

    /**
     * @brief 查找键对应的值（const版本）
     *
     * @param k 键
     * @return 值的const指针，未找到返回nullptr
     */
    const Value* find(Key k) const
    {
        uint32_t si = sparse_index(k);
        if (si >= m_sparse.size())
        {
            return nullptr;
        }
        const SparseEntry& se = m_sparse[si];
        if (se.generation != generation_from_key(k))
        {
            return nullptr;
        }
        if (se.dense_index >= m_dense.size())
        {
            return nullptr;
        }
        return &m_dense[se.dense_index];
    }

    /**
     * @brief 通过键访问值（非const版本）
     *
     * @param k 键
     * @return 值的引用
     */
    Value& operator[](Key k)
    {
        Value* ptr = find(k);
        assert(ptr && "SlotMap key not found");
        return *ptr;
    }

    /**
     * @brief 通过键访问值（const版本）
     *
     * @param k 键
     * @return 值的const引用
     */
    const Value& operator[](Key k) const
    {
        const Value* ptr = find(k);
        assert(ptr && "SlotMap key not found");
        return *ptr;
    }

    /**
     * @brief 获取元素数量
     *
     * @return 元素数量
     */
    uint32_t size() const
    {
        return static_cast<uint32_t>(m_dense.size());
    }

    /**
     * @brief 获取容量
     *
     * @return 容量
     */
    uint32_t capacity() const
    {
        return static_cast<uint32_t>(m_dense.capacity());
    }

    /**
     * @brief 预留空间
     *
     * @param count 要预留的元素数量
     */
    void reserve(uint32_t count)
    {
        m_dense.reserve(count);
        m_dense_keys.reserve(count);
        m_sparse.reserve(count);
    }

    /**
     * @brief 清空所有元素
     *
     * 所有稀疏索引都会被标记为空闲，世代计数器增加。
     *
     * 必须先清空 m_freeList 再重填：旧实现直接 push_back，第二次 clear()
     * 之后自由列表里会出现重复索引（已释放的槽位在上一轮 clear 里进过表，
     * 这一轮又被压进去一次）。后果不止是列表无界增长——allocate_slot 会把
     * 同一个稀疏槽位发出两次，两个不同的 key 指向同一条数据，属正确性缺陷。
     */
    void clear()
    {
        m_dense.clear();
        m_dense_keys.clear();
        m_freeList.clear();
        m_freeList.reserve(m_sparse.size());
        for (uint32_t i = 0; i < m_sparse.size(); ++i)
        {
            bump_generation(m_sparse[i].generation);
            m_sparse[i].dense_index = UINT32_MAX;
            m_freeList.push_back(i);
        }
    }

    /// @name 迭代器接口
    /// @{
    Value* begin()
    {
        return m_dense.data();
    }

    Value* end()
    {
        return m_dense.data() + m_dense.size();
    }

    const Value* begin() const
    {
        return m_dense.data();
    }

    const Value* end() const
    {
        return m_dense.data() + m_dense.size();
    }

    const Value* cbegin() const
    {
        return m_dense.data();
    }

    const Value* cend() const
    {
        return m_dense.data() + m_dense.size();
    }

    /// @}

    /**
     * @brief 获取稠密数组数据指针（非const版本）
     *
     * @return 稠密数组指针
     */
    Value* dense_data()
    {
        return m_dense.data();
    }

    /**
     * @brief 获取稠密数组数据指针（const版本）
     *
     * @return 稠密数组const指针
     */
    const Value* dense_data() const
    {
        return m_dense.data();
    }

    /**
     * @brief 获取稠密数组键数组指针（非const版本）
     *
     * @return 键数组指针
     */
    uint32_t* dense_keys()
    {
        return m_dense_keys.data();
    }

    /**
     * @brief 获取稠密数组键数组指针（const版本）
     *
     * @return 键数组const指针
     */
    const uint32_t* dense_keys() const
    {
        return m_dense_keys.data();
    }
};

/**
 * @brief 图元插槽映射类型别名
 *
 * 使用 uint64_t 作为键类型的 SlotMap。
 */
template<typename Value>
using EntitySlotMap = SlotMap<uint64_t, Value>;
