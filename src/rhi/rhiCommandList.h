/**
 * @file rhiCommandList.h
 * @brief 命令记录接口
 *
 * 旧版 RHI 把绘制状态做成 IDevice 上的即时模式方法
 * （bindPipeline / setUniformMatrix4("uMVP",...) / draw / clear 混在一起），
 * 这是 OpenGL 的全局状态机模型。Vulkan/Metal 没有全局状态机，
 * 只有「录制到命令缓冲」，且 uniform 只能通过 descriptor set / push constant
 * 或 argument buffer 传递，不存在按名字设置的概念。
 *
 * 本接口改为显式的命令记录模型：
 * - 所有绘制必须在 beginRenderPass / endRenderPass 之间
 * - 资源通过预先创建的 BindGroup（对应 descriptor set）整组绑定
 * - 小量高频数据通过 pushConstants 传递（GL 后端映射为一个内部 UBO）
 * - 屏障用后端中立的 BarrierScope 表达
 *
 * GL 后端实现方式：ICommandList 直接转译为立即 GL 调用，
 * submit() 为空操作。这样上层代码在三个后端上语义完全一致。
 */
#pragma once

#include "rhiCore.h"

namespace Render::RHI
{

    class ICommandList
    {
    public:
        virtual ~ICommandList() = default;

        // ---------- 渲染通道 ----------

        /**
         * @brief 开始渲染通道
         *
         * 附件的 load/store 语义与 clear 值在此一次性给定，
         * 取代旧版的 setClearColor + clear + bindRenderTarget 三次即时调用。
         */
        virtual RhiResult beginRenderPass(const RenderPassBeginDesc& desc) = 0;
        virtual void endRenderPass() = 0;

        // ---------- 管线与固定状态 ----------

        virtual void bindPipeline(PipelineHandle pipeline) = 0;
        virtual void setViewport(const Viewport& viewport) = 0;
        virtual void setScissor(const Rect2D& scissor) = 0;

        // ---------- 资源绑定 ----------

        virtual void bindVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offset) = 0;
        virtual void bindIndexBuffer(BufferHandle buffer, uint64_t offset, IndexType type) = 0;

        /// 整组绑定资源。set 索引对应管线声明的 BindingSlot::set。
        virtual void bindBindGroup(uint32_t set, BindGroupHandle group) = 0;

        /**
         * @brief 写入 push constant 块
         *
         * sizeBytes 不得超过管线声明的 pushConstantBytes，
         * 也不得超过 kMaxPushConstantBytes。
         */
        virtual void pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data) = 0;

        // ---------- 绘制 ----------

        virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                          uint32_t firstInstance) = 0;

        virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                 int32_t vertexOffset, uint32_t firstInstance) = 0;

        /// 需 Capabilities::indirectDraw；不支持时返回时无操作。
        virtual void drawIndirect(BufferHandle argBuffer, uint64_t offset, uint32_t drawCount,
                                  uint32_t strideBytes) = 0;

        virtual void drawIndexedIndirect(BufferHandle argBuffer, uint64_t offset, uint32_t drawCount,
                                         uint32_t strideBytes) = 0;

        // ---------- 计算 ----------

        /// 需 Capabilities::computeShaders。必须在 RenderPass 之外调用。
        virtual void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) = 0;

        // ---------- 同步与传输 ----------

        /**
         * @brief 内存屏障
         *
         * before 为「已完成的写入范围」，after 为「即将发生的读取范围」。
         * GL 映射为 glMemoryBarrier(after)，Vulkan 映射为
         * vkCmdPipelineBarrier(srcStage=before, dstStage=after)。
         */
        virtual void barrier(BarrierScope before, BarrierScope after) = 0;

        virtual void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst,
                                uint64_t dstOffset, uint64_t sizeBytes) = 0;

        /// 必须在 RenderPass 之外调用。
        virtual void copyTextureToBuffer(TextureHandle src, BufferHandle dst, uint64_t dstOffset,
                                         const Rect2D& region) = 0;

        // ---------- 调试标记 ----------

        virtual void pushDebugGroup(const char* label) = 0;
        virtual void popDebugGroup() = 0;

        // ---------- 统计 ----------

        virtual FrameStats stats() const = 0;
    };

}  // namespace Render::RHI
