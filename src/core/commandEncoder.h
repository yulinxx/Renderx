/**
 * @file command_encoder.h
 * @brief 统一命令编码器类定义
 *
 * Phase 3 引入的核心组件，负责将 overlay 和 world2D 的绘制命令
 * 收集到同一条内部链路，统一排序后执行。
 *
 * 设计目标：
 * - CPU 侧只负责描述收集
 * - GPU 侧按 batch key 分组，减少状态切换和 draw call
 * - 为后续 MDI / batch 合并 / render graph 预留架构出口
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include <vector>
#include <cstdint>
#include <functional>

namespace Render
{
    namespace core
    {

        // Phase 7: 前向声明管线状态管理器
        class PipelineStateManager;

        // Phase 8: 前向声明绘制合批器
        class DrawBatcher;

        /**
         * @brief 统一命令编码器
         *
         * Phase 3 核心组件，统一收集和排序 overlay / world 绘制命令。
         *
         * 使用流程（每帧）：
         *   1. reset()                    // 清空上一帧命令
         *   2. submitOverlay(...) x N     // OverlayQueue 提交
         *   3. submitWorld(...) x N       // BatchQueue 提交
         *   4. execute(...)               // 统一排序并执行
         */
        class CommandEncoder
        {
        public:
            CommandEncoder();
            ~CommandEncoder();

            /**
             * @brief 初始化编码器
             *
             * 创建内部 pipeline 缓存。
             *
             * @param device RHI 设备指针
             * @return true 初始化成功，false 初始化失败
             */
            bool initialize(RHI::IDevice* device);

            /**
             * @brief 设置管线状态管理器
             *
             * Phase 7 新增。传入 PipelineStateManager 后，编码器会优先通过它
             * 获取/创建和绑定管线，从而复用缓存并过滤冗余绑定。
             *
             * @param psm 管线状态管理器指针（可为 null，表示不使用）
             */
            void setPipelineStateManager(PipelineStateManager* psm);

            /**
             * @brief 设置绘制合批器
             *
             * Phase 8 新增。传入 DrawBatcher 后，编码器会将 overlay 路径的
             * 绘制命令收集到合批器，最终通过 MDI 统一执行，减少 draw call。
             *
             * @param batcher 绘制合批器指针（可为 null，表示不使用）
             */
            void setDrawBatcher(DrawBatcher* batcher);

            /**
             * @brief 关闭并释放资源
             */
            void shutdown();

            /**
             * @brief 重置命令列表
             *
             * 每帧开始时调用，清空上一帧的所有命令。
             */
            void reset();

            /**
             * @brief 提交 overlay 绘制命令
             *
             * 由 OverlayQueue::render() 调用，把 overlay 的绘制意图注册到编码器。
             *
             * @param topology     图元拓扑
             * @param vertexOffset overlay 顶点 buffer 中的偏移
             * @param vertexCount  顶点数量
             * @param zOrder       Z 序（默认 100，确保 overlay 在 world 之上）
             */
            void submitOverlay(
                PrimitiveType topology, uint32_t vertexOffset, uint32_t vertexCount, uint32_t zOrder = 100);

            /**
             * @brief 提交 world2D 绘制命令
             *
             * 由 BatchQueue::render() 调用，把每个 batch 的绘制意图注册到编码器。
             *
             * @param topology      图元拓扑
             * @param materialIndex 材质索引
             * @param indirectOffset 间接命令在 indirect buffer 中的字节偏移
             * @param indirectCount  间接命令数量
             * @param zOrder         Z 序（默认 0）
             * @param lineWidth      线宽（默认 1.0f）
             */
            void submitWorld(PrimitiveType topology,
                uint16_t materialIndex,
                uint32_t indirectOffset,
                uint32_t indirectCount,
                uint32_t zOrder = 0,
                float lineWidth = 1.0f);

            /**
             * @brief 执行所有收集的命令
             *
             * 按 sortKey 排序后，统一绑定 pipeline / buffer 并执行绘制。
             * 相同 topology + material + space 的命令会被连续执行，减少状态切换。
             *
             * @param device      RHI 设备
             * @param worldVB     world2D 顶点 buffer（来自 BatchQueue）
             * @param overlayVB   overlay 顶点 buffer（来自 OverlayQueue）
             * @param indirectBuf 间接命令 buffer（来自 BatchQueue）
             * @param viewMatrix  3x3 视图矩阵
             * @param cameraCenter 相机中心（世界坐标），用于 World2D 相机相对渲染
             */
            void execute(RHI::IDevice* device,
                RHI::BufferHandle worldVB,
                RHI::BufferHandle overlayVB,
                RHI::BufferHandle indirectBuf,
                const float viewMatrix[9],
                const float cameraCenter[2]);

            /**
             * @brief 获取当前命令数量
             */
            uint32_t getCommandCount() const;

            /**
             * @brief 获取上一帧的批次数量（按 sortKey 分组后）
             */
            uint32_t getBatchCount() const
            {
                return m_lastBatchCount;
            }

        private:
            std::vector<DrawCommand> m_commands;
            RHI::IDevice* m_device = nullptr;
            bool m_initialized = false;

            // Phase 7: 管线状态管理器（可选，用于缓存和冗余过滤）
            PipelineStateManager* m_psm = nullptr;

            // Phase 8: 绘制合批器（用于 overlay 路径的 MDI 合批）
            class DrawBatcher* m_drawBatcher = nullptr;

            // pipeline 缓存
            RHI::PipelineHandle m_overlayLinePipeline = {};                   ///< overlay 线段管线
            RHI::PipelineHandle m_overlayTriPipeline = {};                    ///< overlay 三角形管线
            RHI::PipelineHandle m_worldPipelines[PRIMITIVE_TYPE_COUNT] = {};  ///< world 管线

            // 统计
            uint32_t m_lastBatchCount = 0;

            /**
             * @brief 构建排序键
             *
             * 64bit 编码规则（从高到低优先级）：
             *   bits 0-7:   space (0=World2D, 1=Overlay)
             *   bits 8-23:  z-order (uint16, 小值先画)
             *   bits 24-31: topology (PrimitiveType)
             *   bits 32-47: material index
             *   bits 48-63: 保留
             */
            static BatchKey buildSortKey(
                DrawSpace space, uint32_t zOrder, PrimitiveType topology, uint16_t materialIndex);

            /**
             * @brief 获取 overlay 对应的 pipeline
             */
            RHI::PipelineHandle getOverlayPipeline(PrimitiveType topology) const;

            /**
             * @brief 获取 world 对应的 pipeline
             */
            RHI::PipelineHandle getWorldPipeline(PrimitiveType topology) const;

            /**
             * @brief 绑定并执行单个命令
             */
            void dispatchCommand(RHI::IDevice* device,
                const DrawCommand& cmd,
                RHI::BufferHandle worldVB,
                RHI::BufferHandle overlayVB,
                RHI::BufferHandle indirectBuf,
                const float viewMatrix[9]);
        };

    }  // namespace core
}  // namespace render
