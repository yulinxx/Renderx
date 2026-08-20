/**
 * @file render_graph.cpp
 * @brief 显式 Pass 顺序执行器实现
 *
 * Phase 4 引入的核心组件，负责将渲染流程从"隐式调用顺序"升级为
 * "显式 Pass 编排 + 按顺序执行"。
 *
 * 当前定位（Phase 4）：线性顺序执行器（Linear Pass Scheduler）
 * 不执行依赖分析、拓扑排序或自动屏障插入。
 */
#include "render_graph.h"
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        RenderGraph::RenderGraph() = default;
        RenderGraph::~RenderGraph() = default;

        bool RenderGraph::initialize(rhi::IDevice* device)
        {
            if (m_initialized)
            {
                SY_WARNF("RenderGraph::initialize: already initialized");
                return false;
            }
            if (!device)
            {
                SY_ERROR("RenderGraph::initialize: device is null");
                return false;
            }
            m_device = device;
            m_initialized = true;
            m_lastExecutedCount = 0;
            SY_DEBUGF("RenderGraph::initialize: OK");
            return true;
        }

        void RenderGraph::shutdown()
        {
            if (!m_initialized)
            {
                return;
            }

            m_passes.clear();
            m_device = nullptr;
            m_initialized = false;
            m_lastExecutedCount = 0;
            SY_DEBUGF("RenderGraph::shutdown: OK");
        }

        void RenderGraph::addPass(const PassDesc& desc)
        {
            if (!m_initialized)
            {
                SY_ERRORF(
                    "RenderGraph::addPass: not initialized, pass '%s' ignored", desc.name ? desc.name : "(unnamed)");
                return;
            }
            PassEntry entry;
            entry.desc = desc;
            m_passes.push_back(std::move(entry));
            // SY_DEBUGF("RenderGraph::addPass: '%s' (total=%zu)", desc.name ? desc.name : "(unnamed)",
            // m_passes.size());
        }

        void RenderGraph::clear()
        {
            m_passes.clear();
            m_lastExecutedCount = 0;
            // SY_DEBUGF("RenderGraph::clear: all passes removed");
        }

        void RenderGraph::execute(rhi::IDevice* device)
        {
            if (!m_initialized || !device)
            {
                SY_ERROR("RenderGraph::execute: not initialized or device is null");
                return;
            }

            if (m_passes.empty())
            {
                return;
            }

            uint32_t executedCount = 0;
            const uint32_t passCount = static_cast<uint32_t>(m_passes.size());

            for (uint32_t i = 0; i < passCount; ++i)
            {
                const PassEntry& entry = m_passes[i];
                if (!entry.desc.enabled)
                {
                    continue;
                }

                // Phase 4: 先调用 onSetup（如有），设置清屏、深度、混合等状态
                if (entry.desc.onSetup)
                {
                    entry.desc.onSetup(device);
                }

                // Phase 4: 再调用 onExecute，执行实际渲染逻辑
                if (entry.desc.onExecute)
                {
                    entry.desc.onExecute(device);
                }

                ++executedCount;
            }

            m_lastExecutedCount = executedCount;
        }

        uint32_t RenderGraph::getPassCount() const
        {
            return static_cast<uint32_t>(m_passes.size());
        }

        const char* RenderGraph::getPassName(uint32_t index) const
        {
            if (index >= m_passes.size())
            {
                return nullptr;
            }
            return m_passes[index].desc.name;
        }

        void RenderGraph::setPassEnabled(uint32_t index, bool enabled)
        {
            if (index >= m_passes.size())
            {
                SY_WARNF("RenderGraph::setPassEnabled: index %u out of range (count=%zu)", index, m_passes.size());
                return;
            }

            m_passes[index].desc.enabled = enabled;
        }

        bool RenderGraph::isPassEnabled(uint32_t index) const
        {
            if (index >= m_passes.size())
            {
                return false;
            }
            return m_passes[index].desc.enabled;
        }

        void RenderGraph::checkResourceConflicts() const
        {
            // M5: 检查相邻 Pass 之间的资源冲突
            // 仅对已添加的 Pass 进行静态分析，输出警告日志
            for (uint32_t i = 0; i + 1 < m_passes.size(); ++i)
            {
                const PassEntry& prev = m_passes[i];
                const PassEntry& next = m_passes[i + 1];

                // 检查 prev 输出的资源是否被 next 读/写
                for (const auto& out : prev.desc.outputs)
                {
                    for (const auto& in : next.desc.inputs)
                    {
                        if (out.handle != 0 && out.handle == in.handle)
                        {
                        }
                    }
                    for (const auto& out2 : next.desc.outputs)
                    {
                        if (out.handle != 0 && out.handle == out2.handle && out.access == PassResourceAccess::Write)
                        {
                            SY_WARNF("RenderGraph::checkResourceConflicts: [%u:%s] writes and [%u:%s] writes same "
                                     "resource %u (name='%s')",
                                i,
                                prev.desc.name ? prev.desc.name : "",
                                i + 1,
                                next.desc.name ? next.desc.name : "",
                                out.handle,
                                out.name);
                        }
                    }
                }
            }
        }
    }  // namespace core
}  // namespace render