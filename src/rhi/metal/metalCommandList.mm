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

        /**
         * 多边形填充模式。
         *
         * 与 Vulkan/GL 的差别：Metal 的 triangleFillMode 不是管线属性，
         * 而是编码器状态——管线里存不下，只能在每次绑定管线时下发。
         * 因此这里要保证「绑定即下发」，不能因为「管线没换」就跳过。
         */
        MTLTriangleFillMode toMetalFillMode(FillMode mode)
        {
            return mode == FillMode::Wireframe ? MTLTriangleFillModeLines
                                               : MTLTriangleFillModeFill;
        }
    }  // namespace

    MetalCommandList::MetalCommandList(MetalDevice* device)
        : m_device(device)
    {
    }

    void MetalCommandList::beginFrame(MetalSurface* surface, id<MTLCommandBuffer> commandBuffer)
    {
        // 结束上一帧可能遗留的计算编码器（正常情况下 submitFrame/present 已清理，
        // 这里是安全兜底，防止帧被异常跳过后编码器处于打开状态）
        if (m_computeEncoder != nil)
        {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }
        m_surface = surface;
        m_commandBuffer = commandBuffer;
        m_renderEncoder = nil;
        m_computeEncoder = nil;
        m_stats = FrameStats{};
        m_passExtent = Extent2D{};

        // 编码器每帧新建，上一帧的下发结果不会保留：所有绑定状态必须清空，
        // 否则「绑了但没重发」会被误判成有效，画面表现为用错缓冲或丢图元。
        m_pipelineHandle = PipelineHandle{};
        m_pipeline = nullptr;
        m_computePipelineHandle = PipelineHandle{};
        m_computePipeline = nullptr;
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
        m_computeBindGroups.clear();
    }

    RhiResult MetalCommandList::beginRenderPass(const RenderPassBeginDesc& desc)
    {
        if (m_commandBuffer == nil)
        {
            m_device->log().error("[metal] beginRenderPass: no command buffer for this frame "
                                  "(beginFrame not called?)");
            return RhiResult::ErrorNotInitialized;
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
        // 每个 Pass 都是**新的**编码器，上一 Pass 下发的状态一个都不继承
        // （Metal 也不像 GL 那样有跨目标的全局状态）。管线句柄同理必须清空，
        // 否则「同一个管线在第二个 Pass 里再绑一次」会被 bindPipeline 的
        // 去重早退吃掉，新编码器就拿不到 depth-stencil / cull / fillMode，
        // 表现为深度测试失效或线框变实心。GL 侧在 beginRenderPass 做同样的事。
        m_pipelineHandle = PipelineHandle{};
        m_pipeline = nullptr;
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
        // 先尝试渲染管线，再尝试计算管线
        MetalPipelineRecord* renderRecord = m_device->pipelineRecord(pipeline);
        MetalComputePipelineRecord* computeRecord = m_device->computePipelineRecord(pipeline);

        if (renderRecord != nullptr && renderRecord->state != nil)
        {
            // 渲染管线：必须在 RenderPass 内
            if (m_renderEncoder == nil)
            {
                m_device->log().error("[metal] bindPipeline: render pipeline must be bound inside a RenderPass");
                return;
            }
            if (pipeline == m_pipelineHandle && m_pipeline != nullptr)
            {
                return;
            }

            m_pipelineHandle = pipeline;
            m_pipeline = renderRecord;
            m_computePipelineHandle = PipelineHandle{};
            m_computePipeline = nullptr;

            [m_renderEncoder setRenderPipelineState:renderRecord->state];
            if (renderRecord->depthStencilState != nil)
            {
                [m_renderEncoder setDepthStencilState:renderRecord->depthStencilState];
            }
            [m_renderEncoder setCullMode:toMetalCullMode(renderRecord->raster.cullMode)];
            [m_renderEncoder setFrontFacingWinding:toMetalWinding(renderRecord->raster.frontFace)];
            [m_renderEncoder setTriangleFillMode:toMetalFillMode(renderRecord->raster.fillMode)];
            if (renderRecord->raster.depthBiasConstant != 0.0f || renderRecord->raster.depthBiasSlope != 0.0f)
            {
                [m_renderEncoder setDepthBias:renderRecord->raster.depthBiasConstant
                               slopeScale:renderRecord->raster.depthBiasSlope
                                    clamp:0.0f];
            }
            // 顶点布局随管线变化，之前下发的 setVertexBuffer 不再对应新布局
            m_vertexBindingsDirty = true;
            m_stats.pipelineSwitches += 1;
        }
        else if (computeRecord != nullptr && computeRecord->state != nil)
        {
            // 计算管线：必须在 RenderPass 外
            if (m_renderEncoder != nil)
            {
                m_device->log().error("[metal] bindPipeline: compute pipeline must be bound outside RenderPass");
                return;
            }
            if (pipeline == m_computePipelineHandle && m_computePipeline != nullptr)
            {
                return;
            }

            m_computePipelineHandle = pipeline;
            m_computePipeline = computeRecord;
            m_pipelineHandle = PipelineHandle{};
            m_pipeline = nullptr;

            // 确保有 compute encoder
            if (m_computeEncoder == nil)
            {
                m_computeEncoder = [m_commandBuffer computeCommandEncoder];
            }

            [m_computeEncoder setComputePipelineState:computeRecord->state];
            m_stats.pipelineSwitches += 1;
        }
        else
        {
            m_device->log().error("[metal] bindPipeline: invalid pipeline handle");
            m_pipelineHandle = PipelineHandle{};
            m_pipeline = nullptr;
            m_computePipelineHandle = PipelineHandle{};
            m_computePipeline = nullptr;
        }
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
        // 需要在 render pass 内或绑定了 compute 管线
        if (m_renderEncoder == nil && m_computePipeline == nullptr)
        {
            m_device->log().error("[metal] bindBindGroup: must be called inside a RenderPass or with compute pipeline bound");
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

        // 确保 compute bind groups 数组足够大
        if (m_computeBindGroups.size() <= set)
        {
            m_computeBindGroups.resize(set + 1);
        }

        // Metal 不需要预解析名字：(set, binding) 直接展开成参数表 index 下发。
        // 顶点与片元两个 stage 都设一遍——绑定组本身不携带 stage 信息，
        // 只设一个会让资源在另一个 stage 上读到空槽。

        // 渲染管线绑定
        if (m_renderEncoder != nil)
        {
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
        }

        // 计算管线绑定
        if (m_computePipeline != nullptr && m_computeEncoder != nil)
        {
            // 确保有 compute encoder
            for (const BufferBinding& entry : record->buffers)
            {
                MetalBufferRecord* buffer = m_device->bufferRecord(entry.buffer);
                if (buffer == nullptr || buffer->buffer == nil)
                {
                    continue;
                }
                const uint32_t index = toMetalBufferIndex(entry.set, entry.binding);
                const NSUInteger offset = static_cast<NSUInteger>(entry.offset);
                [m_computeEncoder setBuffer:buffer->buffer offset:offset atIndex:index];
            }

            for (const TextureBinding& entry : record->textures)
            {
                MetalTextureRecord* texture = m_device->textureRecord(entry.texture);
                if (texture == nullptr || texture->texture == nil)
                {
                    continue;
                }
                const uint32_t index = toMetalTextureIndex(entry.set, entry.binding);
                [m_computeEncoder setTexture:texture->texture atIndex:index];

                MetalSamplerRecord* sampler = m_device->samplerRecord(entry.sampler);
                if (sampler != nullptr && sampler->sampler != nil)
                {
                    [m_computeEncoder setSamplerState:sampler->sampler atIndex:index];
                }
            }

            m_computeBindGroups[set] = group;
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

        if (!m_device->capabilities().indirectDraw)
        {
            m_device->log().error("[metal] drawIndirect: indirect draw not supported");
            return;
        }
        if (!prepareDraw("drawIndirect"))
        {
            return;
        }

        MetalBufferRecord* args = m_device->bufferRecord(argsBuffer);
        if (args == nullptr || args->buffer == nil)
        {
            m_device->log().error("[metal] drawIndirect: invalid args buffer");
            return;
        }

        [m_renderEncoder drawPrimitives:toMetalTopology(m_pipeline->topology)
                            indirectBuffer:args->buffer
                     indirectBufferOffset:static_cast<NSUInteger>(offsetBytes)];
        m_stats.drawCalls += drawCount;
    }

    void MetalCommandList::drawIndexedIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                               uint32_t drawCount, uint32_t strideBytes)
    {
        (void)strideBytes;

        if (!m_device->capabilities().indirectDraw)
        {
            m_device->log().error("[metal] drawIndexedIndirect: indirect draw not supported");
            return;
        }
        if (!prepareDraw("drawIndexedIndirect"))
        {
            return;
        }

        MetalBufferRecord* args = m_device->bufferRecord(argsBuffer);
        if (args == nullptr || args->buffer == nil)
        {
            m_device->log().error("[metal] drawIndexedIndirect: invalid args buffer");
            return;
        }

        MetalBufferRecord* index = m_device->bufferRecord(m_indexBuffer);
        if (index == nullptr || index->buffer == nil)
        {
            m_device->log().error("[metal] drawIndexedIndirect: no valid index buffer bound");
            return;
        }

        const MTLIndexType type = m_indexType == IndexType::Uint16 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32;

        // 索引数/索引起点/顶点偏移/起始实例全部来自 args 缓冲的
        // MTLDrawIndexedPrimitivesIndirectArguments；索引缓冲本身与基址偏移
        // 仍由调用提供（indirect.indexStart 会按索引尺寸自动叠加到基址上）。
        [m_renderEncoder drawIndexedPrimitives:toMetalTopology(m_pipeline->topology)
                                      indexType:type
                                    indexBuffer:index->buffer
                              indexBufferOffset:static_cast<NSUInteger>(m_indexOffset)
                                  indirectBuffer:args->buffer
                            indirectBufferOffset:static_cast<NSUInteger>(offsetBytes)];
        m_stats.drawCalls += drawCount;
    }

    void MetalCommandList::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        // 计算着色器必须在渲染通道之外调用
        if (m_renderEncoder != nil)
        {
            m_device->log().error("[metal] dispatchCompute: must be called outside RenderPass");
            return;
        }
        if (m_computePipeline == nullptr)
        {
            m_device->log().error("[metal] dispatchCompute: no compute pipeline bound");
            return;
        }

        // 如果还没有 compute encoder，创建一个
        if (m_computeEncoder == nil)
        {
            m_computeEncoder = [m_commandBuffer computeCommandEncoder];
        }

        [m_computeEncoder setComputePipelineState:m_computePipeline->state];

        // 设置 push constants（计算着色器也使用同样的 push constant 机制）
        if (m_pushConstantsDirty && m_pushConstantHighWater > 0)
        {
            [m_computeEncoder setBytes:m_pushConstants length:m_pushConstantHighWater atIndex:kMetalPushConstantIndex];
            m_pushConstantsDirty = false;
        }

        // 分发计算
        const MTLSize threadgroupSize = MTLSizeMake(
            (std::min)(groupsX, static_cast<uint32_t>(m_computePipeline->state.maxTotalThreadsPerThreadgroup)),
            (std::min)(groupsY, static_cast<uint32_t>(m_computePipeline->state.maxTotalThreadsPerThreadgroup / groupsX)),
            (std::min)(groupsZ, static_cast<uint32_t>(m_computePipeline->state.maxTotalThreadsPerThreadgroup / (groupsX * groupsY)))
        );
        const MTLSize threadgroupsCount = MTLSizeMake(groupsX, groupsY, groupsZ);
        [m_computeEncoder dispatchThreadgroups:threadgroupsCount threadsPerThreadgroup:threadgroupSize];

        m_stats.computeDispatches += 1;
    }

    void MetalCommandList::endComputePass()
    {
        if (m_computeEncoder != nil)
        {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }
    }

    void MetalCommandList::barrier(BarrierScope before, BarrierScope after)
    {
        // Metal 在同一个编码器内部自动保证顺序，只需要处理跨编码器的情况
        if (before == BarrierScope::None || after == BarrierScope::None)
        {
            return;
        }

        // 需要在 compute encoder 和 render encoder 之间做同步
        // 结束当前的 encoder 并在需要时插入 barrier
        if (m_computeEncoder != nil && m_renderEncoder == nil)
        {
            // 从 compute 到其他阶段的 barrier - 结束 compute encoder
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }
        else if (m_renderEncoder != nil)
        {
            // 从 render 到其他阶段的 barrier - 结束 render encoder
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
        }

        // 注意：Metal 的完整 barrier 支持需要使用 MTLSharedEvent 或在 compute encoder
        // 中使用 useResource:usage:stages: 来同步。对于大多数 CAD 使用场景，
        // 简单地结束和重新开始 encoder 已经足够。
        (void)before;
        (void)after;
    }

    void MetalCommandList::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst,
                                      uint64_t dstOffset, uint64_t sizeBytes)
    {
        if (sizeBytes == 0)
        {
            return;
        }
        if (m_commandBuffer == nil)
        {
            m_device->log().error("[metal] copyBuffer: no command buffer");
            return;
        }

        MetalBufferRecord* srcRecord = m_device->bufferRecord(src);
        MetalBufferRecord* dstRecord = m_device->bufferRecord(dst);
        if (srcRecord == nullptr || srcRecord->buffer == nil || dstRecord == nullptr || dstRecord->buffer == nil)
        {
            m_device->log().error("[metal] copyBuffer: invalid src or dst buffer");
            return;
        }

        // 如果当前有 render encoder，需要先结束它
        if (m_renderEncoder != nil)
        {
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
        }
        // 如果当前有 compute encoder，需要先结束它
        if (m_computeEncoder != nil)
        {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }

        // 创建 blit encoder 并执行拷贝
        id<MTLBlitCommandEncoder> blit = [m_commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:srcRecord->buffer
               sourceOffset:static_cast<NSUInteger>(srcOffset)
                   toBuffer:dstRecord->buffer
          destinationOffset:static_cast<NSUInteger>(dstOffset)
                       size:static_cast<NSUInteger>(sizeBytes)];
        [blit endEncoding];
    }

    void MetalCommandList::copyTextureToBuffer(TextureHandle src, BufferHandle dst,
                                               uint64_t dstOffset, const Rect2D& region)
    {
        if (m_commandBuffer == nil)
        {
            m_device->log().error("[metal] copyTextureToBuffer: no command buffer");
            return;
        }

        MetalTextureRecord* srcRecord = m_device->textureRecord(src);
        MetalBufferRecord* dstRecord = m_device->bufferRecord(dst);
        if (srcRecord == nullptr || srcRecord->texture == nil || dstRecord == nullptr || dstRecord->buffer == nil)
        {
            m_device->log().error("[metal] copyTextureToBuffer: invalid src texture or dst buffer");
            return;
        }

        // 如果当前有 render encoder，需要先结束它
        if (m_renderEncoder != nil)
        {
            [m_renderEncoder endEncoding];
            m_renderEncoder = nil;
        }
        if (m_computeEncoder != nil)
        {
            [m_computeEncoder endEncoding];
            m_computeEncoder = nil;
        }

        // 创建 blit encoder 并执行拷贝
        id<MTLBlitCommandEncoder> blit = [m_commandBuffer blitCommandEncoder];

        MTLOrigin srcOrigin = MTLOriginMake(region.x, region.y, 0);
        MTLSize srcSize = MTLSizeMake(region.width, region.height, 1);

        [blit copyFromTexture:srcRecord->texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:srcOrigin
                    sourceSize:srcSize
                      toBuffer:dstRecord->buffer
             destinationOffset:static_cast<NSUInteger>(dstOffset)
              destinationBytesPerRow:region.width * 4  // 假设 RGBA8
           destinationBytesPerImage:region.width * region.height * 4];
        [blit endEncoding];
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
