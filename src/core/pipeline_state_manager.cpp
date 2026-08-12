/**
 * @file pipeline_state_manager.cpp
 * @brief 管线状态管理器实现
 */
#include "pipeline_state_manager.h"
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        uint32_t PipelineStateManager::hashString(const char* str)
        {
            if (!str)
                return 0;
            // FNV-1a 32-bit
            uint32_t h = 2166136261u;
            while (*str)
            {
                h ^= static_cast<uint8_t>(*str);
                h *= 16777619u;
                ++str;
            }
            return h;
        }

        bool PipelineStateManager::initialize(rhi::IDevice* device)
        {
            if (!device)
            {
                SY_ERROR("[PipelineStateManager] initialize: device is null");
                return false;
            }
            m_device = device;
            m_currentPipeline = rhi::NullHandle;
            SY_DEBUG("[PipelineStateManager] initialized");
            return true;
        }

        void PipelineStateManager::shutdown()
        {
            clearCache();
            m_device = nullptr;
            m_currentPipeline = rhi::NullHandle;
            SY_DEBUG("[PipelineStateManager] shutdown");
        }

        rhi::PipelineHandle PipelineStateManager::getOrCreatePipeline(const rhi::PipelineDesc& desc)
        {
            if (!m_device)
            {
                SY_ERROR("[PipelineStateManager] getOrCreatePipeline: device is null");
                return rhi::NullHandle;
            }

            // 从 desc 构建状态键
            PipelineStateKey key;
            key.vsNameHash = hashString(desc.vertexShader);
            key.fsNameHash = hashString(desc.fragmentShader);
            key.csNameHash = hashString(desc.computeShader);
            key.topology = static_cast<uint8_t>(desc.topology);
            key.vertexFormat = static_cast<uint8_t>(desc.vertexFormat);
            key.depthTest = desc.depthTest ? 1u : 0u;
            key.depthWrite = desc.depthWrite ? 1u : 0u;
            key.blendEnable = desc.blendEnable ? 1u : 0u;
            key.depthFunc = static_cast<uint8_t>(desc.depthFunc);
            key.srcBlend = static_cast<uint8_t>(desc.srcBlend);
            key.dstBlend = static_cast<uint8_t>(desc.dstBlend);

            // 查找缓存
            auto it = m_cache.find(key);
            if (it != m_cache.end())
            {
                return it->second;
            }

            // 缓存未命中，创建新管线
            rhi::PipelineHandle handle = m_device->createPipeline(desc);
            if (handle == rhi::NullHandle)
            {
                SY_ERROR("[PipelineStateManager] failed to create pipeline");
                return rhi::NullHandle;
            }

            m_cache[key] = handle;
            m_entries.push_back({ key, handle });

            SY_DEBUGF("[PipelineStateManager] new pipeline created, cache size=%zu", m_cache.size());
            return handle;
        }

        void PipelineStateManager::bindPipeline(rhi::PipelineHandle pipeline)
        {
            if (!m_device)
                return;

            if (pipeline == rhi::NullHandle)
                return;

            // 冗余过滤：目标管线与当前已绑定管线相同则跳过
            if (pipeline == m_currentPipeline)
                return;

            m_device->bindPipeline(pipeline);
            m_currentPipeline = pipeline;
        }

        void PipelineStateManager::clearCache()
        {
            if (!m_device)
            {
                m_cache.clear();
                m_entries.clear();
                return;
            }

            for (const auto& entry : m_entries)
            {
                if (entry.handle != rhi::NullHandle)
                    m_device->destroyPipeline(entry.handle);
            }

            m_cache.clear();
            m_entries.clear();
            m_currentPipeline = rhi::NullHandle;
        }
    } // namespace core
} // namespace render