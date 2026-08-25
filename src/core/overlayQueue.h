/**
 * @file OverlayQueue.h
 * @brief 叠加层渲染队列 —— 通用、可扩展的绘制命令队列
 *
 * 设计原则：
 * - DLL 只管「怎么画」：顶点缓冲区管理、GPU 上传、Draw 调用
 * - 应用层定义「画什么」：语义、分组、Z 序、拓扑
 * - 无硬编码图元类型，完全由应用层通过 Group + DrawDesc 描述
 *
 * 核心数据结构：
 * - 统一顶点缓冲区 (GPU_CPU_Coherent)，环形复用
 * - DrawRange：单个绘制命令的元数据
 * - Group：应用层定义的生命周期分组，仅用于增量清除
 */
#pragma once

#include "render/RenderTypes.h"
#include "../rhi/rhiDevice.h"
#include <vector>
#include <cstdint>
#include <array>
#include <unordered_map>

namespace Render
{
    namespace core
    {
        class CommandEncoder;

        /**
         * @brief 绘制范围描述 —— 一个完整的绘制命令
         *
         * 由应用层提交，DLL 负责排序和执行。
         */
        struct DrawRange
        {
            uint32_t vertexOffset = 0;     ///< 顶点缓冲区偏移 (顶点数)
            uint32_t vertexCount = 0;      ///< 顶点数量
            PrimitiveType topology = PrimitiveType::TriangleList; ///< 拓扑类型
            uint32_t group = 0;            ///< 生命周期分组 (应用层定义，0=默认)
            float zOrder = 0.0f;           ///< Z 序，用于排序
            bool isTriangle = false;       ///< true=TriangleList, false=LineList/Strip
        };

        /**
         * @brief 叠加层渲染队列
         *
         * 统一管理所有 overlay 顶点数据和绘制命令。
         * 支持增量更新：按 Group 粒度标记脏数据，仅上传变化部分。
         */
        class OverlayQueue
        {
        public:
            // --- 公共类型定义 (需在方法前声明) ---

            /**
             * @brief 绘制范围描述 —— 单个绘制命令的完整元数据
             *
             * 应用层填充此结构，DLL 只负责存储顶点、记录范围、排序和执行。
             * 无硬编码图元类型，完全由应用层通过 topology + group + zOrder 描述。
             */
            struct DrawRange
            {
                uint32_t vertexOffset = 0;     ///< 顶点缓冲区偏移 (顶点数)
                uint32_t vertexCount = 0;      ///< 顶点数量
                PrimitiveType topology = PrimitiveType::TriangleList; ///< 拓扑类型
                uint32_t group = 0;            ///< 生命周期分组 (应用层定义，0=默认)
                float zOrder = 0.0f;           ///< Z 序，用于排序
                bool isTriangle = false;       ///< true=TriangleList, false=LineList/Strip
            };

            /**
             * @brief 批量提交项
             */
            struct DrawItem
            {
                const OverlayVertex* vertices;
                uint32_t vertexCount;
                DrawRange range;
            };

            // --- 公共方法 ---

            /**
             * @brief 初始化叠加层渲染队列
             * @param device RHI 设备指针
             * @param initialCapacity 初始顶点缓冲区容量 (顶点数)，默认 4096
             * @return true 初始化成功
             */
            bool initialize(RHI::IDevice* device, uint32_t initialCapacity = 4096);

            /**
             * @brief 关闭并释放所有资源
             */
            void shutdown();

            /**
             * @brief 提交绘制命令 (统一入口)
             *
             * 应用层调用此接口提交任意 overlay 几何数据。
             * DLL 不关心语义，只负责存储顶点和记录绘制范围。
             *
             * @param vertices 顶点数据指针
             * @param vertexCount 顶点数量
             * @param range 绘制范围描述 (拓扑、group、zOrder 等)
             * @return 分配的 vertexOffset，失败返回 UINT32_MAX
             */
            uint32_t submit(const OverlayVertex* vertices, uint32_t vertexCount, const DrawRange& range);

            /**
             * @brief 批量提交多个绘制命令
             *
             * @param items 绘制项数组，每项包含顶点数据和范围描述
             * @param count 项数量
             * @return 首个分配的 vertexOffset，失败返回 UINT32_MAX
             */
            uint32_t submitBatch(const DrawItem* items, uint32_t count);

            /**
             * @brief 清除指定 Group 的所有绘制命令
             *
             * 标记该 Group 为脏，下一帧 render() 时会从缓冲区移除并压缩。
             *
             * @param group 要清除的分组 ID (应用层定义)
             */
            void clearGroup(uint32_t group);

            /**
             * @brief 清除所有绘制命令
             */
            void clearAll();

            /**
             * @brief 渲染所有 overlay
             *
             * 执行流程：
             * 1. 若有脏数据，重建/增量更新 GPU 顶点缓冲区
             * 2. 按 zOrder 排序所有 DrawRange
             * 3. 通过 CommandEncoder 批量提交绘制命令
             *
             * @param device RHI 设备
             * @param encoder 命令编码器
             */
            void render(RHI::IDevice* device, CommandEncoder* encoder);

            /**
             * @brief 获取顶点缓冲区句柄 (供 CommandEncoder 绑定)
             */
            RHI::BufferHandle getVertexBuffer() const { return m_vertexBuffer; }

            /**
             * @brief 当前有效的绘制命令数量
             */
            uint32_t getDrawCount() const { return static_cast<uint32_t>(m_ranges.size()); }

            /**
             * @brief 设置最大顶点缓冲区容量 (自动扩容)
             */
            void setMaxCapacity(uint32_t capacity) { m_maxCapacity = capacity; }

            private:
            // 内部范围记录 (包含运行时状态)
            struct InternalRange : DrawRange
            {
                bool alive = true;  // 标记是否有效 (clearGroup 后置 false)
            };

            RHI::IDevice* m_device = nullptr;
            RHI::BufferHandle m_vertexBuffer = RHI::NullHandle;
            uint32_t m_vbCapacity = 0;           // 当前 GPU 缓冲区容量
            uint32_t m_maxCapacity = 65536;      // 最大允许容量
            uint32_t m_writeOffset = 0;          // 环形缓冲区写入位置

            std::vector<OverlayVertex> m_stagingBuffer; // CPU 侧暂存缓冲区
            std::vector<InternalRange> m_ranges;        // 所有绘制范围
            std::unordered_map<uint32_t, std::vector<uint32_t>> m_groupIndices; // group -> range indices

            // 脏标记：每个 group 一个标记
            std::unordered_map<uint32_t, bool> m_groupDirty;

            // 统计
            uint32_t m_totalVertices = 0;

            // --- 内部方法 ---
            bool ensureCapacity(uint32_t requiredVertices);
            void uploadStagingBuffer();
            void rebuildStagingBuffer();      // 全量重建 (clearGroup 后)
            void compactRanges();              // 移除死范围，压缩 staging buffer
            void sortRangesByZOrder();         // 按 zOrder 排序
            void submitToEncoder(CommandEncoder* encoder);
        };

    }  // namespace core
}  // namespace Render