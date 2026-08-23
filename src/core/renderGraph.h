/**
 * @file RenderGraph.h
 * @brief 显式 Pass 调度层定义
 *
 * Phase 4 引入的核心组件，负责将渲染流程从"隐式调用顺序"升级为
 * "显式 Pass 编排 + 按顺序执行"。
 *
 * 当前定位（Phase 4）：显式 Pass 顺序执行器（Linear Pass Scheduler）
 *   - 按添加顺序依次执行 Pass
 *   - 支持 enable/disable 控制
 *   - 支持 onSetup（状态设置）和 onExecute（实际渲染）回调
 *
 * 尚未实现（留给后续 Phase）：
 *   - Pass 依赖分析（拓扑排序）
 *   - 资源读写冲突检查
 *   - 自动屏障（barrier）插入
 *   - 多分支并行执行
 *
 * 设计原则：
 * - 最小可用：先只承载顺序、目标、清屏和资源访问信息
 * - 不破坏现有行为：Pass 内部仍调用原有渲染逻辑
 * - 预留扩展：资源访问声明（inputs/outputs）为后续屏障管理留出口
 */
#pragma once

#include "rhi/rhiDevice.h"
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

namespace Render
{
    namespace core
    {

        // ============================================================================
        // 资源访问语义（为后续屏障管理预留）
        // ============================================================================

        /**
         * @brief Pass 对资源的访问类型
         */
        enum class PassResourceAccess : uint8_t
        {
            None = 0,
            Read = 1 << 0,   // 读访问
            Write = 1 << 1,  // 写访问
            ReadWrite = Read | Write,
        };

        /**
         * @brief Pass 可能访问的资源类型
         */
        enum class PassResourceType : uint8_t
        {
            ColorTarget,     // 颜色目标
            DepthTarget,     // 深度目标
            VertexBuffer,    // 顶点缓冲
            IndexBuffer,     // 索引缓冲
            UniformBuffer,   // 统一变量缓冲
            Texture,         // 纹理
            IndirectBuffer,  // 间接绘制命令缓冲
        };

        /**
         * @brief 资源槽描述（用于依赖追踪）
         */
        struct PassResourceSlot
        {
            PassResourceType type;      // 资源类型
            PassResourceAccess access;  // 访问方式
            const char* name;           // 调试名称
            uint32_t handle;            // RHI 句柄（可选，0 表示未指定）
        };

        // ============================================================================
        // Pass 描述
        // ============================================================================

        /**
         * @brief 单个渲染 Pass 的描述
         *
         * 一个 Pass 代表渲染流程中的一个阶段，例如：
         * - FrameSetup：设置清屏颜色、深度测试、混合状态
         * - SceneEnv：渲染背景网格
         * - World2DCollect：收集 2D 文档几何绘制命令到 CommandEncoder
         * - OverlayCollect：收集叠加层绘制命令到 CommandEncoder
         * - CommandExecute：执行已收集的所有绘制命令
         * - Text：渲染文本
         *
         * Pass 按 addPass() 的调用顺序执行，不做拓扑排序。
         */
        struct PassDesc
        {
            const char* name = nullptr;  // Pass 名称（用于日志和调试）
            bool enabled = true;         // 是否启用

            // 状态设置回调（可选）：设置清屏颜色、深度测试、混合等
            std::function<void(RHI::IDevice*)> onSetup;

            // 执行回调（必须）：实际渲染逻辑
            std::function<void(RHI::IDevice*)> onExecute;

            // 输入资源声明（读访问）
            std::vector<PassResourceSlot> inputs;

            // 输出资源声明（写访问）
            std::vector<PassResourceSlot> outputs;
        };

        // ============================================================================
        // 渲染图
        // ============================================================================

        /**
         * @brief 显式 Pass 顺序执行器
         *
         * 管理一组按添加顺序执行的渲染 Pass，提供：
         * - 显式的执行顺序（不再依赖代码中的隐式调用顺序）
         * - Pass 级别的启用/禁用控制
         * - 资源访问声明（为后续自动屏障管理做准备）
         * - 执行统计和日志观测
         *
         * 注意（Phase 4 当前状态）：
         * - 这是线性顺序执行器，不是完整的依赖图调度器
         * - 不执行依赖分析、拓扑排序或自动屏障插入
         * - 这些能力将在后续 Phase 中逐步引入
         */
        class RenderGraph
        {
        public:
            RenderGraph();
            ~RenderGraph();

            /**
             * @brief 初始化渲染图
             *
             * @param device RHI 设备指针
             * @return true 初始化成功，false 初始化失败
             */
            bool initialize(RHI::IDevice* device);

            /**
             * @brief 关闭并释放资源
             */
            void shutdown();

            /**
             * @brief 添加一个 Pass（按添加顺序执行）
             */
            void addPass(const PassDesc& desc);

            /**
             * @brief 清空所有 Pass
             */
            void clear();

            /**
             * @brief 按顺序执行所有启用的 Pass
             *
             * 执行流程：
             *   1. 遍历所有 Pass
             *   2. 对启用的 Pass 先调用 onSetup（如有）
             *   3. 再调用 onExecute
             *   4. 记录执行统计
             */
            void execute(RHI::IDevice* device);

            /**
             * @brief 获取 Pass 数量
             */
            uint32_t getPassCount() const;

            /**
             * @brief 按索引获取 Pass 名称
             */
            const char* getPassName(uint32_t index) const;

            /**
             * @brief 启用/禁用指定 Pass
             */
            void setPassEnabled(uint32_t index, bool enabled);

            /**
             * @brief 查询 Pass 是否启用
             */
            bool isPassEnabled(uint32_t index) const;

            /**
             * @brief 获取上一帧实际执行的 Pass 数量
             */
            uint32_t getExecutedPassCount() const
            {
                return m_lastExecutedCount;
            }

            /**
             * @brief M5: 检查相邻 Pass 之间是否存在资源冲突
             *
             * 分析相邻两个 Pass 的资源访问是否有写-读或读-写冲突。
             * 输出日志警告但不改变执行顺序。
             * 后续将用于自动插入屏障或重新排序 Pass。
             */
            void checkResourceConflicts() const;

        private:
            struct PassEntry
            {
                PassDesc desc;
            };

            std::vector<PassEntry> m_passes;
            RHI::IDevice* m_device = nullptr;
            bool m_initialized = false;
            uint32_t m_lastExecutedCount = 0;
        };

    }  // namespace core
}  // namespace Render
