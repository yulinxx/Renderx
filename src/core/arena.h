#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <vector>

class Arena {
    std::vector<uint8_t> m_buffer;
    size_t m_offset;

    static size_t align_up(size_t value, size_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

public:
    explicit Arena(size_t blockSize = 64 * 1024)
        : m_buffer(blockSize), m_offset(0) {}

    void* allocate(size_t size, size_t alignment = alignof(std::max_align_t)) {
        size_t aligned = align_up(m_offset, alignment);
        if (aligned + size > m_buffer.size()) return nullptr;
        m_offset = aligned + size;
        return m_buffer.data() + aligned;
    }

    template<typename T, typename... Args>
    T* emplace(Args&&... args) {
        void* ptr = allocate(sizeof(T), alignof(T));
        if (!ptr) return nullptr;
        return new (ptr) T(static_cast<Args&&>(args)...);
    }

    void reset() { m_offset = 0; }

    size_t used()     const { return m_offset; }
    size_t capacity() const { return m_buffer.size(); }
};
