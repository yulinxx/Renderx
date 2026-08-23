/**
 * @file drawBatcher.h
 * @brief 绘制合批器
 *
 * Phase 8 新增。对统一提交后的命令做排序、分组和合批，
 * 生成 Multi-Draw-Indirect 命令以减少 draw call。
 *
 * 当前实现聚焦于 overlay 路径的合批：
 * - overlay 命令共享同一顶点缓冲，仅偏移和数量不同
 * - 相同 pipeline 的连续命令被合并为一个 indirect draw
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include <vector>
#include <cstdint>

namespace Render
{
    namespace core
    {

        /**
         * @brief 合批后的绘制组描述
         *
         * 每个 BatchGroup 对应一次 drawIndirect 调用。
         */
        struct BatchGroup
        {
            RHI::PipelineHandle pipeline;    ///< 要绑定的管线
            uint32_t indirectOffset;         ///< indirect buffer 中的字节偏移
            uint32_t drawCount;              ///< 间接命令数量
            DrawSpace space;                 ///< 绘制空间（用于区分 buffer 绑定）
            RHI::BufferHandle vertexBuffer;  ///< 该组使用的顶点缓冲
        };

        /**
         * @brief 绘制合批器
         *
         * 接收已排序的 DrawCommand 数组，按 pipeline 分组并生成 indirect 命令缓冲。
         *
         * 使用流程（每帧）：
         *   1. reset()                              // 清空上一帧数据
         *   2. appendOverlayCmd(...) x N            // 追加 overlay 命令
         *   3. build()                              // 生成分组和 indirect buffer
         *   4. for each group: bindPipeline + drawIndirect
         */
        class DrawBatcher
        {
        public:
            DrawBatcher() = default;

            ~DrawBatcher()
            {
                shutdown();
            }

            /**
             * @brief 初始化合批器
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
             * @brief 重置帧状态
             */
            void reset();

            /**
             * @brief 追加 overlay 绘制命令
             *
             * @param pipeline     命令使用的管线
             * @param vertexOffset 顶点缓冲偏移
             * @param vertexCount  顶点数量
             */
            void appendOverlayCmd(RHI::PipelineHandle pipeline, uint32_t vertexOffset, uint32_t vertexCount);

            /**
             * @brief 构建合批结果
             *
             * 将已追加的命令按 pipeline 分组，生成 indirect buffer，
             * 并输出 BatchGroup 数组供调用方遍历执行。
             *
             * @return 合批后的绘制组数组引用
             */
            const std::vector<BatchGroup>& build();

            /**
             * @brief 获取 indirect buffer 句柄
             */
            RHI::BufferHandle getIndirectBuffer() const
            {
                return m_indirectBuffer;
            }

            /**
             * @brief 获取原始命令数量
             */
            uint32_t getCommandCount() const
            {
                return m_cmdCount;
            }

            /**
             * @brief 获取合批后的组数量
             */
            uint32_t getGroupCount() const
            {
                return static_cast<uint32_t>(m_groups.size());
            }

        private:
            RHI::IDevice* m_device = nullptr;
            bool m_initialized = false;

            /// 原始 indirect 命令数组
            std::vector<DrawIndirectCmd> m_indirectCmds;
            /// 每个命令对应的 pipeline（用于分组）
            std::vector<RHI::PipelineHandle> m_cmdPipelines;
            /// 合批后的绘制组
            std::vector<BatchGroup> m_groups;
            /// indirect GPU buffer
            RHI::BufferHandle m_indirectBuffer = RHI::NullHandle;
            /// buffer 容量（命令数）
            uint32_t m_bufferCapacity = 0;
            /// 原始命令计数
            uint32_t m_cmdCount = 0;
        };

    }  // namespace core
}  // namespace Render
