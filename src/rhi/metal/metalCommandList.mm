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
#include <cstring>

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

        // 编码器每帧新建，上一帧的下发结果不会保留：所有绑定状态必须清空，
        // 否则「绑了但没重发」会被误判成有效，画面表现为用错缓冲或丢图元。
        m_pipelineHandle = PipelineHandle{};
        m_pipeline = nullptr;
        for (VertexBinding& binding : m_vertexBindings)
        {
            binding.buffer = BufferHandle{};
            binding.offset = 0;
        }
        m_vertexBindingsDirty = false;
        m_indexBuffer = BufferHandle{};
        m_indexOffset = 0;
        m_pushConstantHighWater = 0;
        m_pushConstantsDirty = false;
    }

    RhiResult MetalCommandList::beginRenderPass(const RenderPassBeginDesc& desc)
    {
        if (m_commandBuffer == nil)
        {
            m_device->log().error("[metal] beginRenderPass: no command buffer for this frame "
                                  "(beginFrame not called?)");
            return RhiResult::ErrorInvalidHandle;
        }
        if (m_renderEncoder != nil)
        {
            m_device->log().error("[metal] beginRenderPass: previous render pass is still open");
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
                m_device->log().error("[metal] beginRenderPass: color attachment %u has no texture "
                                      "(was the back buffer acquired?)", i);
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
                m_device->log().error("[metal] beginRenderPass: depth attachment declared but no "
                                      "texture is available");
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
            m_device->log().error("[metal] beginRenderPass: renderCommandEncoder creation failed");
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
            m_device->log().warn("[metal] endRenderPass: no render pass is currently open");
            return;
        }
        [m_renderEncoder endEncoding];
        m_renderEncoder = nil;
    }

    void MetalCommandList::bindPipeline(PipelineHandle pipeline)
    {
        if (pipeline == m_pipelineHandle && m_pipeline != nullptr)
        {
            return;
        }
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] bindPipeline: must be called inside a RenderPass");
            return;
        }

        MetalPipelineRecord* record = m_device->pipelineRecord(pipeline);
        if (record == nullptr || record->state == nil)
        {
            m_device->log().error("[metal] bindPipeline: invalid pipeline handle");
            // 与 GL 一致：清掉当前绑定，避免后续 draw 复用旧管线（旧拓扑/旧顶点布局）
            // 照画不误——那会表现为"图元错位"却一条错误日志都没有。
            m_pipelineHandle = PipelineHandle{};
            m_pipeline = nullptr;
            return;
        }

        m_pipelineHandle = pipeline;
        m_pipeline = record;
        [m_renderEncoder setRenderPipelineState:record->state];
        if (record->depthStencilState != nil)
        {
            [m_renderEncoder setDepthStencilState:record->depthStencilState];
        }
        [m_renderEncoder setCullMode:toMetalCullMode(record->raster.cullMode)];
        [m_renderEncoder setFrontFacingWinding:toMetalWinding(record->raster.frontFace)];
        if (record->raster.depthBiasConstant != 0.0f || record->raster.depthBiasSlope != 0.0f)
        {
            [m_renderEncoder setDepthBias:record->raster.depthBiasConstant
                               slopeScale:record->raster.depthBiasSlope
                                    clamp:0.0f];
        }
        // 顶点布局随管线变化，之前下发的 setVertexBuffer 不再对应新布局
        m_vertexBindingsDirty = true;
        m_stats.pipelineSwitches += 1;
    }

    void MetalCommandList::setViewport(const Viewport& viewport)
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] setViewport: must be called inside a RenderPass");
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
            m_device->log().error("[metal] setScissor: must be called inside a RenderPass");
            return;
        }
        // Metal 要求裁剪矩形必须落在附件范围内，越界会直接断言失败，
        // 因此这里主动钳制到本帧的 pass 尺寸。
        const uint32_t maxWidth = m_passExtent.width;
        const uint32_t maxHeight = m_passExtent.height;
        if (rect.x < 0 || rect.y < 0 || maxWidth == 0 || maxHeight == 0)
        {
            m_device->log().warn("[metal] setScissor: rect out of range or pass extent unknown; skipped");
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
        if (slot >= kMaxVertexBufferSlots)
        {
            m_device->log().error("[metal] bindVertexBuffer: slot=%u exceeds limit %u", slot,
                                  kMaxVertexBufferSlots);
            return;
        }
        if (m_vertexBindings[slot].buffer == buffer && m_vertexBindings[slot].offset == offsetBytes)
        {
            return;
        }
        m_vertexBindings[slot].buffer = buffer;
        m_vertexBindings[slot].offset = offsetBytes;
        m_vertexBindingsDirty = true;
    }

    void MetalCommandList::bindIndexBuffer(BufferHandle buffer, uint64_t offsetBytes, IndexType type)
    {
        // Metal 把索引缓冲作为 drawIndexedPrimitives 的参数传入，没有独立的绑定状态；
        // 这里只记录，绘制时随调用一起下发。
        m_indexBuffer = buffer;
        m_indexOffset = offsetBytes;
        m_indexType = type;
    }

    void MetalCommandList::bindBindGroup(uint32_t set, BindGroupHandle group)
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] bindBindGroup: must be called inside a RenderPass");
            return;
        }
        if (set >= kMaxDescriptorSets)
        {
            m_device->log().error("[metal] bindBindGroup: set=%u exceeds limit %u", set, kMaxDescriptorSets);
            return;
        }
        MetalBindGroupRecord* record = m_device->bindGroupRecord(group);
        if (record == nullptr)
        {
            m_device->log().error("[metal] bindBindGroup: invalid bind group handle");
            return;
        }

        // Metal 不需要预解析名字：(set, binding) 直接展开成参数表 index 下发。
        // 顶点与片元两个 stage 都设一遍——绑定组本身不携带 stage 信息，
        // 只设一个会让资源在另一个 stage 上读到空槽。
        for (const BufferBinding& entry : record->buffers)
        {
            MetalBufferRecord* buffer = m_device->bufferRecord(entry.buffer);
            if (buffer == nullptr || buffer->buffer == nil)
            {
                m_device->log().warn("[metal] bindBindGroup: set=%u binding=%u has no valid buffer", set,
                                     entry.binding);
                continue;
            }
            const uint32_t index = toMetalBufferIndex(entry.set, entry.binding);
            if (index >= kMetalPushConstantIndex)
            {
                m_device->log().error("[metal] bindBindGroup: buffer index %u collides with the "
                                      "push-constant slot (set=%u binding=%u)",
                                      index, entry.set, entry.binding);
                continue;
            }
            const NSUInteger offset = static_cast<NSUInteger>(entry.offset);
            [m_renderEncoder setVertexBuffer:buffer->buffer offset:offset atIndex:index];
            [m_renderEncoder setFragmentBuffer:buffer->buffer offset:offset atIndex:index];
        }

        for (const TextureBinding& entry : record->textures)
        {
            MetalTextureRecord* texture = m_device->textureRecord(entry.texture);
            if (texture == nullptr || texture->texture == nil)
            {
                m_device->log().warn("[metal] bindBindGroup: set=%u binding=%u has no valid texture", set,
                                     entry.binding);
                continue;
            }
            const uint32_t index = toMetalTextureIndex(entry.set, entry.binding);
            [m_renderEncoder setVertexTexture:texture->texture atIndex:index];
            [m_renderEncoder setFragmentTexture:texture->texture atIndex:index];

            MetalSamplerRecord* sampler = m_device->samplerRecord(entry.sampler);
            if (sampler != nullptr && sampler->sampler != nil)
            {
                [m_renderEncoder setVertexSamplerState:sampler->sampler atIndex:index];
                [m_renderEncoder setFragmentSamplerState:sampler->sampler atIndex:index];
            }
        }

        m_stats.bindGroupSwitches += 1;
    }

    void MetalCommandList::pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data)
    {
        if (data == nullptr || sizeBytes == 0)
        {
            return;
        }
        if (offsetBytes + sizeBytes > kMaxPushConstantBytes)
        {
            m_device->log().error("[metal] pushConstants: offset=%u size=%u exceeds kMaxPushConstantBytes=%u",
                                  offsetBytes, sizeBytes, kMaxPushConstantBytes);
            return;
        }
        std::memcpy(m_pushConstants + offsetBytes, data, sizeBytes);
        if (offsetBytes + sizeBytes > m_pushConstantHighWater)
        {
            m_pushConstantHighWater = offsetBytes + sizeBytes;
        }
        m_pushConstantsDirty = true;
    }

    void MetalCommandList::flushVertexBindings()
    {
        if (!m_vertexBindingsDirty || m_pipeline == nullptr || m_renderEncoder == nil)
        {
            return;
        }
        for (uint32_t slot = 0; slot < kMaxVertexBufferSlots; ++slot)
        {
            MetalBufferRecord* buffer = m_device->bufferRecord(m_vertexBindings[slot].buffer);
            if (buffer == nullptr || buffer->buffer == nil)
            {
                continue;
            }
            [m_renderEncoder setVertexBuffer:buffer->buffer
                                      offset:static_cast<NSUInteger>(m_vertexBindings[slot].offset)
                                     atIndex:slot];
        }
        m_vertexBindingsDirty = false;
    }

    void MetalCommandList::flushPushConstants()
    {
        if (!m_pushConstantsDirty || m_pipeline == nullptr || m_renderEncoder == nil)
        {
            return;
        }
        // 管线声明了多大的 pushConstant 块就发多大：声明 0 表示该管线不用它
        const uint32_t bytes = (std::min)(m_pushConstantHighWater, m_pipeline->pushConstantBytes);
        if (bytes == 0)
        {
            return;
        }
        // setBytes 是拷贝语义，源指针只需在本次调用期间有效。两个 stage 都设，
        // 因为顶点与片元 shader 可能读同一个块（如 uView 与 uViewport）。
        [m_renderEncoder setVertexBytes:m_pushConstants length:bytes atIndex:kMetalPushConstantIndex];
        [m_renderEncoder setFragmentBytes:m_pushConstants length:bytes atIndex:kMetalPushConstantIndex];
        m_pushConstantsDirty = false;
    }

    bool MetalCommandList::prepareDraw(const char* what)
    {
        if (m_renderEncoder == nil)
        {
            m_device->log().error("[metal] %s: must be called between beginRenderPass / endRenderPass", what);
            return false;
        }
        if (m_pipeline == nullptr)
        {
            m_device->log().error("[metal] %s: bindPipeline must be called first", what);
            return false;
        }
        flushVertexBindings();
        flushPushConstants();
        return true;
    }

    void MetalCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                                uint32_t firstInstance)
    {
        if (vertexCount == 0 || !prepareDraw("draw"))
        {
            return;
        }
        [m_renderEncoder drawPrimitives:toMetalTopology(m_pipeline->topology)
                            vertexStart:firstVertex
                            vertexCount:vertexCount
                          instanceCount:(instanceCount == 0 ? 1 : instanceCount)
                           baseInstance:firstInstance];
        m_stats.drawCalls += 1;
    }

    void MetalCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount,
                                       uint32_t firstIndex, int32_t vertexOffset,
                                       uint32_t firstInstance)
    {
        if (indexCount == 0 || !prepareDraw("drawIndexed"))
        {
            return;
        }
        MetalBufferRecord* index = m_device->bufferRecord(m_indexBuffer);
        if (index == nullptr || index->buffer == nil)
        {
            m_device->log().error("[metal] drawIndexed: no valid index buffer bound");
            return;
        }

        const MTLIndexType type = m_indexType == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;
        const uint32_t indexSize = m_indexType == IndexType::Uint16 ? 2u : 4u;
        const uint64_t offset = m_indexOffset + static_cast<uint64_t>(firstIndex) * indexSize;

        [m_renderEncoder drawIndexedPrimitives:toMetalTopology(m_pipeline->topology)
                                    indexCount:indexCount
                                     indexType:type
                                   indexBuffer:index->buffer
                             indexBufferOffset:static_cast<NSUInteger>(offset)
                                 instanceCount:(instanceCount == 0 ? 1 : instanceCount)
                                    baseVertex:vertexOffset
                                  baseInstance:firstInstance];
        m_stats.drawCalls += 1;
    }

    void MetalCommandList::drawIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                        uint32_t drawCount, uint32_t strideBytes)
    {
        (void)argsBuffer;
        (void)offsetBytes;
        (void)drawCount;
        (void)strideBytes;
        m_device->log().error("[metal] drawIndirect is not implemented yet (M3)");
    }

    void MetalCommandList::drawIndexedIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                               uint32_t drawCount, uint32_t strideBytes)
    {
        (void)argsBuffer;
        (void)offsetBytes;
        (void)drawCount;
        (void)strideBytes;
        m_device->log().error("[metal] drawIndexedIndirect is not implemented yet (M3)");
    }

    void MetalCommandList::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        (void)groupsX;
        (void)groupsY;
        (void)groupsZ;
        m_device->log().error("[metal] dispatchCompute is not implemented yet (M3)");
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
        m_device->log().error("[metal] copyBuffer is not implemented yet (M3)");
    }

    void MetalCommandList::copyTextureToBuffer(TextureHandle src, BufferHandle dst,
                                               uint64_t dstOffset, const Rect2D& region)
    {
        (void)src;
        (void)dst;
        (void)dstOffset;
        (void)region;
        m_device->log().error("[metal] copyTextureToBuffer is not implemented yet (M3)");
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
