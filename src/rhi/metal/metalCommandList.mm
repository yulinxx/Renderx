/**
 * @file metalCommandList.mm
 * @brief Metal 后端：命令记录器的实现（仅 Apple 平台编译）
 *
 * 覆盖 M1 需要的部分：RenderPass、视口、裁剪、调试标记、屏障。
 * 绘制与计算入口在 M2/M3 补齐，当前一律记录 error 并跳过——
 * 与 GL 后端的「缺能力就明确报错」保持同一纪律。
 *
 * 两处与 GL 后端的坐标系差异必须记住：
 * 1. Metal 的视口/裁剪原点在**左上角**，与窗口坐标一致；GL 是左下角，
 *    因此 GlCommandList 里那些 y 翻转在 Metal 侧全部不需要。
 * 2. 片元阶段不做 y 翻转，读回时也不需要（readTexture 里已说明）。
 */

#include "rhi/metal/metalDevice.h"

#include <algorithm>

namespace Render::RHI::metal
{

    namespace
    {
        MTLLoadAction toLoadAction(LoadOp op)
        {
            switch (op)
            {
            case LoadOp::Load: return MTLLoadActionLoad;
            case LoadOp::Clear: return MTLLoadActionClear;
            case LoadOp::DontCare: return MTLLoadActionDontCare;
            }
            return MTLLoadActionClear;
        }

        MTLStoreAction toStoreAction(StoreOp op)
        {
            return op == StoreOp::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
        }
    }  // namespace

    MetalCommandList::MetalCommandList(MetalDevice* device)
        : m_device(device)
    {
    }

    void MetalCommandList::beginFrame(MetalSurface* surface, id<MTLCommandBuffer> commandBuffer)
    {
        m_surface = surface;
        m_commandBuffer = commandBuffer;
        m_renderEncoder = nil;
        m_stats = FrameStats{};
        m_passExtent = Extent2D{};
    }

    RhiResult MetalCommandList::beginRenderPass(const RenderPassBeginDesc& desc)
    {
        if (m_commandBuffer == nil)
        {
            m_device->log().error("[metal] beginRenderPass: 本帧没有命令缓冲（beginFrame 未调用？）");
            return RhiResult::ErrorInvalidHandle;
        }
        if (m_renderEncoder != nil)
        {
            m_device->log().error("[metal] beginRenderPass: 上一个 RenderPass 未结束");
            return RhiResult::ErrorUnknown;
        }

        MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];

        for (uint32_t i = 0; i < desc.colorAttachmentCount && i < kMaxColorAttachments; ++i)
        {
            const ColorAttachment& src = desc.colorAttachments[i];
            id<MTLTexture> texture = nil;
            if (src.texture.valid())
            {
                MetalTextureRecord* record = m_device->textureRecord(src.texture);
                texture = record != nullptr ? record->texture : nil;
            }
            else if (m_surface != nullptr)
            {
                // 附件留空表示「画到表面」：Metal 侧即当前 drawable 的纹理
                texture = m_surface->currentDrawableTexture();
            }

            if (texture == nil)
            {
                m_device->log().error("[metal] beginRenderPass: 颜色附件 %u 没有可用纹理"
                                      "（历史缓冲可能尚未 acquire）", i);
                return RhiResult::ErrorInvalidArgument;
            }

            MTLRenderPassColorAttachmentDescriptor* color = pass.colorAttachments[i];
            color.texture = texture;
            color.loadAction = toLoadAction(src.loadOp);
            color.storeAction = toStoreAction(src.storeOp);
            color.clearColor = MTLClearColorMake(src.clearValue.r, src.clearValue.g,
                                                 src.clearValue.b, src.clearValue.a);
        }

        if (desc.hasDepthAttachment)
        {
            id<MTLTexture> depth = nil;
            if (desc.depthAttachment.texture.valid())
            {
                MetalTextureRecord* record = m_device->textureRecord(desc.depthAttachment.texture);
                depth = record != nullptr ? record->texture : nil;
            }
            else if (m_surface != nullptr)
            {
                MetalTextureRecord* record = m_device->textureRecord(m_surface->depthTexture());
                depth = record != nullptr ? record->texture : nil;
            }

            if (depth == nil)
            {
                m_device->log().error("[metal] beginRenderPass: 声明了深度附件但没有可用纹理");
                return RhiResult::ErrorInvalidArgument;
            }

            MTLRenderPassDepthAttachmentDescriptor* attachment = pass.depthAttachment;
            attachment.texture = depth;
            attachment.loadAction = toLoadAction(desc.depthAttachment.loadOp);
            attachment.storeAction = toStoreAction(desc.depthAttachment.storeOp);
            attachment.clearDepth = desc.depthAttachment.clearDepth;
        }

        m_renderEncoder = [m_commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] beginRenderPass: renderCommandEncoder 创建失败");
            return RhiResult::ErrorUnknown;
        }

        m_passExtent = desc.extent;
        if (desc.debugName != nullptr)
        {
            // 用 label 而不是 pushDebugGroup：调试组必须成对 pop，
            // 而 RenderPass 的结束是对应 endEncoding，不是 popDebugGroup。
            m_renderEncoder.label = [NSString stringWithUTF8String:desc.debugName];
        }
        return RhiResult::Ok;
    }

    void MetalCommandList::endRenderPass()
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().warn("[metal] endRenderPass: 当前没有进行中的 RenderPass");
            return;
        }
        [m_renderEncoder endEncoding];
        m_renderEncoder = nil;
    }

    void MetalCommandList::bindPipeline(PipelineHandle pipeline)
    {
        (void)pipeline;
        m_device->log().error("[metal] bindPipeline 尚未实现（M2）");
    }

    void MetalCommandList::setViewport(const Viewport& viewport)
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] setViewport: 必须在 RenderPass 内调用");
            return;
        }
        // Metal 的视口原点在左上角，与传入值同一约定，不做 y 翻转
        MTLViewport native{};
        native.originX = viewport.x;
        native.originY = viewport.y;
        native.width = viewport.width;
        native.height = viewport.height;
        native.znear = viewport.minDepth;
        native.zfar = viewport.maxDepth;
        [m_renderEncoder setViewport:native];
    }

    void MetalCommandList::setScissor(const Rect2D& rect)
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] setScissor: 必须在 RenderPass 内调用");
            return;
        }
        // Metal 要求裁剪矩形必须落在附件范围内，越界会直接断言失败，
        // 因此这里主动钳制到本帧的 pass 尺寸。
        const uint32_t maxWidth = m_passExtent.width;
        const uint32_t maxHeight = m_passExtent.height;
        if (rect.x < 0 || rect.y < 0 || maxWidth == 0 || maxHeight == 0)
        {
            m_device->log().warn("[metal] setScissor: 矩形越界或 pass 尺寸未知，已跳过");
            return;
        }

        const uint32_t x = static_cast<uint32_t>(rect.x);
        const uint32_t y = static_cast<uint32_t>(rect.y);
        if (x >= maxWidth || y >= maxHeight)
        {
            // 完全在附件之外：用 0 尺寸矩形表达「裁掉一切」
            MTLScissorRect empty{};
            empty.x = 0;
            empty.y = 0;
            empty.width = 0;
            empty.height = 0;
            [m_renderEncoder setScissorRect:empty];
            return;
        }

        MTLScissorRect native{};
        native.x = x;
        native.y = y;
        native.width = (std::min)(rect.width, maxWidth - x);
        native.height = (std::min)(rect.height, maxHeight - y);
        [m_renderEncoder setScissorRect:native];
    }

    void MetalCommandList::bindVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offsetBytes)
    {
        (void)slot;
        (void)buffer;
        (void)offsetBytes;
        m_device->log().error("[metal] bindVertexBuffer 尚未实现（M2）");
    }

    void MetalCommandList::bindIndexBuffer(BufferHandle buffer, uint64_t offsetBytes, IndexType type)
    {
        (void)buffer;
        (void)offsetBytes;
        (void)type;
        m_device->log().error("[metal] bindIndexBuffer 尚未实现（M2）");
    }

    void MetalCommandList::bindBindGroup(uint32_t set, BindGroupHandle group)
    {
        (void)set;
        (void)group;
        m_device->log().error("[metal] bindBindGroup 尚未实现（M2）");
    }

    void MetalCommandList::pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data)
    {
        (void)offsetBytes;
        (void)sizeBytes;
        (void)data;
        m_device->log().error("[metal] pushConstants 尚未实现（M2）");
    }

    void MetalCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                                uint32_t firstInstance)
    {
        (void)vertexCount;
        (void)instanceCount;
        (void)firstVertex;
        (void)firstInstance;
        m_device->log().error("[metal] draw 尚未实现（M2）");
    }

    void MetalCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                       uint32_t firstIndex, int32_t vertexOffset,
                                       uint32_t firstInstance)
    {
        (void)indexCount;
        (void)instanceCount;
        (void)firstIndex;
        (void)vertexOffset;
        (void)firstInstance;
        m_device->log().error("[metal] drawIndexed 尚未实现（M2）");
    }

    void MetalCommandList::drawIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                        uint32_t drawCount, uint32_t strideBytes)
    {
        (void)argsBuffer;
        (void)offsetBytes;
        (void)drawCount;
        (void)strideBytes;
        m_device->log().error("[metal] drawIndirect 尚未实现（M3）");
    }

    void MetalCommandList::drawIndexedIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                               uint32_t drawCount, uint32_t strideBytes)
    {
        (void)argsBuffer;
        (void)offsetBytes;
        (void)drawCount;
        (void)strideBytes;
        m_device->log().error("[metal] drawIndexedIndirect 尚未实现（M3）");
    }

    void MetalCommandList::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        (void)groupsX;
        (void)groupsY;
        (void)groupsZ;
        m_device->log().error("[metal] dispatchCompute 尚未实现（M3）");
    }

    void MetalCommandList::barrier(BarrierScope before, BarrierScope after)
    {
        // Metal 在同一个编码器内部自动保证先后顺序，不需要显式屏障；
        // 跨编码器（如 compute 与渲染之间）才需要 MTLBarrier，随 M3 补。
        (void)before;
        (void)after;
    }

    void MetalCommandList::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst,
                                      uint64_t dstOffset, uint64_t sizeBytes)
    {
        (void)src;
        (void)srcOffset;
        (void)dst;
        (void)dstOffset;
        (void)sizeBytes;
        m_device->log().error("[metal] copyBuffer 尚未实现（M3）");
    }

    void MetalCommandList::copyTextureToBuffer(TextureHandle src, BufferHandle dst,
                                               uint64_t dstOffset, const Rect2D& region)
    {
        (void)src;
        (void)dst;
        (void)dstOffset;
        (void)region;
        m_device->log().error("[metal] copyTextureToBuffer 尚未实现（M3）");
    }

    void MetalCommandList::pushDebugGroup(const char* name)
    {
        if (m_renderEncoder == nil || name == nullptr)
        {
            return;
        }
        [m_renderEncoder pushDebugGroup:[NSString stringWithUTF8String:name]];
    }

    void MetalCommandList::popDebugGroup()
    {
        if (m_renderEncoder == nil)
        {
            return;
        }
        [m_renderEncoder popDebugGroup];
    }

}  // namespace Render::RHI::metal
