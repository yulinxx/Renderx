#pragma once

#include <cassert>
#include <cstdint>
#include <vector>

template<typename Key, typename Value>
class SlotMap {
    static_assert(sizeof(Key) == sizeof(uint64_t), "Key must be 64-bit");

    struct SparseEntry {
        uint32_t dense_index;
        uint32_t generation;
    };

    std::vector<Value>       m_dense;
    std::vector<uint32_t>    m_dense_keys;
    std::vector<SparseEntry> m_sparse;
    std::vector<uint32_t>    m_freeList;

    static uint32_t sparse_index(Key k) {
        return static_cast<uint32_t>(k & 0xFFFFFFFF);
    }

    static uint32_t generation_from_key(Key k) {
        return static_cast<uint32_t>(k >> 32);
    }

    static Key make_key(uint32_t generation, uint32_t index) {
        return static_cast<Key>(static_cast<uint64_t>(generation) << 32) |
               static_cast<Key>(index);
    }

public:
    Key insert(const Value& v) {
        uint32_t sparse_idx;
        if (!m_freeList.empty()) {
            sparse_idx = m_freeList.back();
            m_freeList.pop_back();
            m_sparse[sparse_idx].generation += 1;
            m_sparse[sparse_idx].dense_index = static_cast<uint32_t>(m_dense.size());
        } else {
            sparse_idx = static_cast<uint32_t>(m_sparse.size());
            m_sparse.push_back({static_cast<uint32_t>(m_dense.size()), 0});
        }
        m_dense.push_back(v);
        m_dense_keys.push_back(sparse_idx);
        return make_key(m_sparse[sparse_idx].generation, sparse_idx);
    }

    Key insert(Value&& v) {
        uint32_t sparse_idx;
        if (!m_freeList.empty()) {
            sparse_idx = m_freeList.back();
            m_freeList.pop_back();
            m_sparse[sparse_idx].generation += 1;
            m_sparse[sparse_idx].dense_index = static_cast<uint32_t>(m_dense.size());
        } else {
            sparse_idx = static_cast<uint32_t>(m_sparse.size());
            m_sparse.push_back({static_cast<uint32_t>(m_dense.size()), 0});
        }
        m_dense.push_back(std::move(v));
        m_dense_keys.push_back(sparse_idx);
        return make_key(m_sparse[sparse_idx].generation, sparse_idx);
    }

    void erase(Key k) {
        uint32_t si = sparse_index(k);
        assert(si < m_sparse.size());
        SparseEntry& se = m_sparse[si];
        assert(se.generation == generation_from_key(k));

        uint32_t di = se.dense_index;
        uint32_t last = static_cast<uint32_t>(m_dense.size() - 1);

        if (di != last) {
            m_dense[di] = std::move(m_dense[last]);
            m_dense_keys[di] = m_dense_keys[last];
            m_sparse[m_dense_keys[di]].dense_index = di;
        }

        m_dense.pop_back();
        m_dense_keys.pop_back();
        se.generation += 1;
        se.dense_index = UINT32_MAX;
        m_freeList.push_back(si);
    }

    Value* find(Key k) {
        uint32_t si = sparse_index(k);
        if (si >= m_sparse.size()) return nullptr;
        const SparseEntry& se = m_sparse[si];
        if (se.generation != generation_from_key(k)) return nullptr;
        if (se.dense_index >= m_dense.size()) return nullptr;
        return &m_dense[se.dense_index];
    }

    const Value* find(Key k) const {
        uint32_t si = sparse_index(k);
        if (si >= m_sparse.size()) return nullptr;
        const SparseEntry& se = m_sparse[si];
        if (se.generation != generation_from_key(k)) return nullptr;
        if (se.dense_index >= m_dense.size()) return nullptr;
        return &m_dense[se.dense_index];
    }

    Value& operator[](Key k) {
        Value* ptr = find(k);
        assert(ptr && "SlotMap key not found");
        return *ptr;
    }

    const Value& operator[](Key k) const {
        const Value* ptr = find(k);
        assert(ptr && "SlotMap key not found");
        return *ptr;
    }

    uint32_t size() const { return static_cast<uint32_t>(m_dense.size()); }
    uint32_t capacity() const { return static_cast<uint32_t>(m_dense.capacity()); }

    void reserve(uint32_t count) {
        m_dense.reserve(count);
        m_dense_keys.reserve(count);
        m_sparse.reserve(count);
    }

    void clear() {
        m_dense.clear();
        m_dense_keys.clear();
        for (uint32_t i = 0; i < m_sparse.size(); ++i) {
            m_sparse[i].generation += 1;
            m_sparse[i].dense_index = UINT32_MAX;
            m_freeList.push_back(i);
        }
    }

    Value*       begin()        { return m_dense.data(); }
    Value*       end()          { return m_dense.data() + m_dense.size(); }
    const Value* begin()  const { return m_dense.data(); }
    const Value* end()    const { return m_dense.data() + m_dense.size(); }
    const Value* cbegin() const { return m_dense.data(); }
    const Value* cend()   const { return m_dense.data() + m_dense.size(); }

    Value*       dense_data()       { return m_dense.data(); }
    const Value* dense_data() const { return m_dense.data(); }

    uint32_t*       dense_keys()       { return m_dense_keys.data(); }
    const uint32_t* dense_keys() const { return m_dense_keys.data(); }
};

template<typename Value>
using EntitySlotMap = SlotMap<uint64_t, Value>;
