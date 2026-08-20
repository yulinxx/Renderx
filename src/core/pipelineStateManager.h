/**
 * @file pipeline_state_manager.h
 * @brief 管线状态管理器
 *
 * Phase 7 新增。负责缓存和复用 RHI 管线对象，减少重复创建开销。
 * 提供统一的管线获取与绑定接口，并自动过滤冗余绑定。
 */
#pragma once

#include "../rhi/rhi_device.h"
#include "../rhi/rhi_types.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
#include <vector>

namespace render
{
    namespace core
    {

        /**
         * @brief 管线状态键
         *
         * 将 RHI PipelineDesc 的所有状态参数编码为一个紧凑的可哈希结构。
         * 相同状态组合的 key 能稳定命中缓存，避免重复创建 pipeline。
         */
        struct PipelineStateKey
        {
            uint32_t vsNameHash = 0;   ///< 顶点着色器名称哈希
            uint32_t fsNameHash = 0;   ///< 片段着色器名称哈希
            uint32_t csNameHash = 0;   ///< 计算着色器名称哈希（可选）
            uint8_t topology = 0;      ///< PrimitiveTopology 枚举值
            uint8_t vertexFormat = 0;  ///< VertexFormat 枚举值
            uint8_t depthTest : 1;     ///< 是否启用深度测试
            uint8_t depthWrite : 1;    ///< 是否启用深度写入
            uint8_t blendEnable : 1;   ///< 是否启用混合
            uint8_t depthFunc : 4;     ///< CompareFunc 枚举值
            uint8_t srcBlend = 0;      ///< BlendFactor 枚举值（源）
            uint8_t dstBlend = 0;      ///< BlendFactor 枚举值（目标）
            uint8_t reserved = 0;      ///< 保留对齐

            bool operator==(const PipelineStateKey& other) const
            {
                return std::memcmp(this, &other, sizeof(*this)) == 0;
            }
        };

    }  // namespace core
}  // namespace render

namespace std
{

    template<>
    struct hash<render::core::PipelineStateKey>
    {
        size_t operator()(const render::core::PipelineStateKey& k) const noexcept
        {
            // FNV-1a 风格：把 key 当字节流整体哈希
            size_t h = 14695981039346656037ull;
            const uint8_t* p = reinterpret_cast<const uint8_t*>(&k);
            for (size_t i = 0; i < sizeof(k); ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
            return h;
        }
    };

}  // namespace std

namespace render
{
    namespace core
    {

        /**
         * @brief 管线状态缓存项
         *
         * 保存已创建的 RHI 管线句柄及其对应的状态键。
         */
        struct PipelineCacheEntry
        {
            PipelineStateKey key;
            rhi::PipelineHandle handle;
        };

        /**
         * @brief 管线状态管理器
         *
         * 封装 DSA 风格的零绑定管线切换，提供以下能力：
         * - 根据 PipelineDesc 生成状态键并查找缓存
         * - 缓存未命中时自动创建 RHI 管线
         * - 绑定管线时自动过滤与当前已绑定管线的冗余切换
         * - 关闭时统一销毁所有缓存的管线对象
         */
        class PipelineStateManager
        {
        public:
            PipelineStateManager() = default;
            ~PipelineStateManager() = default;

            /**
             * @brief 初始化管理器
             *
             * @param device RHI 设备指针
             * @return true 初始化成功，false 初始化失败
             */
            bool initialize(rhi::IDevice* device);

            /**
             * @brief 关闭并释放所有缓存的管线
             */
            void shutdown();

            /**
             * @brief 获取或创建管线
             *
             * 根据状态描述查找缓存，命中则直接返回；
             * 未命中则通过 RHI 创建新管线并加入缓存。
             *
             * @param desc 管线状态描述
             * @return RHI 管线句柄
             */
            rhi::PipelineHandle getOrCreatePipeline(const rhi::PipelineDesc& desc);

            /**
             * @brief 绑定管线（带冗余过滤）
             *
             * 仅在目标管线与当前已绑定管线不同时执行 RHI 绑定操作。
             *
             * @param pipeline 要绑定的管线句柄
             */
            void bindPipeline(rhi::PipelineHandle pipeline);

            /**
             * @brief 获取当前已绑定的管线
             */
            rhi::PipelineHandle currentPipeline() const
            {
                return m_currentPipeline;
            }

            /**
             * @brief 强制设置当前管线记录（用于外部已直接绑定管线的场景）
             *
             * @param pipeline 外部已绑定的管线句柄
             */
            void forceSetCurrentPipeline(rhi::PipelineHandle pipeline)
            {
                m_currentPipeline = pipeline;
            }

            /**
             * @brief 重置当前管线状态（用于外部直接绑定管线后同步PSM状态）
             *
             * SceneEnv::render() 等路径直接调用 device->bindPipeline() 绑定管线，
             * 绕过 PSM，导致 PSM 缓存的 m_currentPipeline 与 GPU 实际状态不一致。
             * 在 CommandEncoder::execute() 开始时调用此方法，强制 PSM 在首次
             * bindPipeline 时重新执行 GPU 绑定，避免拓扑类型错误（如 LineLoop
             * 被错误地沿用为 LineList，导致六边形只显示交替的三条边）。
             */
            void resetCurrentPipeline()
            {
                m_currentPipeline = rhi::NullHandle;
            }

            /**
             * @brief 获取缓存统计
             *
             * @return 当前缓存的管线数量
             */
            size_t cacheSize() const
            {
                return m_cache.size();
            }

            /**
             * @brief 清空所有缓存管线
             */
            void clearCache();

        private:
            rhi::IDevice* m_device = nullptr;
            rhi::PipelineHandle m_currentPipeline = rhi::NullHandle;

            std::unordered_map<PipelineStateKey, rhi::PipelineHandle> m_cache;
            std::vector<PipelineCacheEntry> m_entries;  // 用于有序遍历和日志输出

            static uint32_t hashString(const char* str);
        };

    }  // namespace core
}  // namespace render
