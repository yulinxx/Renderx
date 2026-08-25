/**
 * @file rhiResourcePool.h
 * @brief RHI 资源句柄池（世代式，防悬垂）
 *
 * 所有后端共用同一套句柄语义，避免每个后端各写一遍 vector + freeList：
 * 旧 rhiGl 用「句柄 = 下标 + 1」，销毁后下标立即复用，被释放的旧句柄会
 * 静默命中新资源——这类 use-after-free 不会崩溃，只会画错，极难定位。
 *
 * 本池基于 core/slotMap.h 的世代机制：句柄高 32 位是世代号，低 32 位是槽位，
 * 世代号从 1 起算，因此句柄永不为 0（0 恒为「无效」）。销毁后世代自增，
 * 旧句柄的 get() 返回 nullptr。
 */
#pragma once

#include "core/slotMap.h"
#include "rhiCore.h"

#include <utility>

namespace Render::RHI
{

    /**
     * @brief 强类型句柄 → 记录 的映射
     *
     * @tparam HandleT 形如 BufferHandle / TextureHandle 的 Handle<Tag>
     * @tparam Record  后端私有的资源记录类型（如 GL 的 { GLuint name; ... }）
     */
    template <typename HandleT, typename Record>
    class ResourcePool
    {
    public:
        /// 插入记录并返回新句柄；句柄保证非 0
        HandleT add(Record&& record)
        {
            HandleT h{};
            h.value = m_map.insert(std::move(record));
            return h;
        }

        HandleT add(const Record& record)
        {
            HandleT h{};
            h.value = m_map.insert(record);
            return h;
        }

        /// 查找记录；句柄无效或世代不匹配（已销毁）时返回 nullptr
        Record* get(HandleT handle)
        {
            if (!handle.valid())
            {
                return nullptr;
            }
            return m_map.find(handle.value);
        }

        const Record* get(HandleT handle) const
        {
            if (!handle.valid())
            {
                return nullptr;
            }
            return m_map.find(handle.value);
        }

        /**
         * @brief 移除记录
         *
         * @return true 表示句柄有效且已移除；false 表示句柄无效或已被移除
         *         （调用方可据此记录一条「重复销毁」日志，而不是静默通过）
         */
        bool remove(HandleT handle)
        {
            if (!get(handle))
            {
                return false;
            }
            m_map.erase(handle.value);
            return true;
        }

        uint32_t size() const { return m_map.size(); }

        /// 遍历全部存活记录，用于设备关闭时释放底层 GPU 对象
        Record* begin() { return m_map.begin(); }
        Record* end() { return m_map.end(); }

        void clear() { m_map.clear(); }

    private:
        SlotMap<uint64_t, Record> m_map;
    };

}  // namespace Render::RHI
