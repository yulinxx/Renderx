/**
 * @file glCommandList.cpp
 * @brief GL 后端：命令记录器实现
 *
 * GL 是即时 API，本文件负责把「命令记录 + RenderPass + 描述符组」的语义
 * 映射到即时调用上，并集中处理三件旧实现散落各处的事：
 *  - 状态冗余消除（管线切换时一次性下发，而非每次绘制重设）
 *  - 坐标系翻转（RHI 用左上角原点，GL 视口/裁剪用左下角）
 *  - 绘制前置校验（未 bindPipeline / 未 beginRenderPass 直接绘制会被拦下并记错）
 */

#include "rhi/gl/glDevice.h"

#include <cstring>

#ifndef GL_TEXTURE_2D
    #define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_FRAMEBUFFER
    #define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_COPY_READ_BUFFER
    #define GL_COPY_READ_BUFFER 0x8F36
#endif
#ifndef GL_COPY_WRITE_BUFFER
    #define GL_COPY_WRITE_BUFFER 0x8F37
#endif
#ifndef GL_PIXEL_PACK_BUFFER
    #define GL_PIXEL_PACK_BUFFER 0x88EB
#endif

namespace Render::RHI::gl
{

    GlCommandList::GlCommandList(GlDevice* device) : m_device(device) {}

    void GlCommandList::beginFrame(GlSurface* surface)
    {
        m_surface = surface;
        m_stats = FrameStats{};
        m_inRenderPass = false;
        m_pipelineHandle = PipelineHandle{};
        for (auto& binding : m_vertexBindings)
        {
            binding = VertexBinding{};
        }
        m_vertexBindingsDirty = false;
        m_indexBuffer = BufferHandle{};
        m_indexOffset = 0;
        m_indexType = IndexType::Uint32;
        // pushConstant 内容不跨帧保留：上层每帧写入 view 矩阵等，
        // 保留旧值会掩盖「忘记写」的错误。
        std::memset(m_pushConstants, 0, sizeof(m_pushConstants));
        m_pushConstantHighWater = 0;
        m_pushConstantsDirty = false;
    }

    GLuint GlCommandList::resolveFramebuffer(const RenderPassBeginDesc& desc)
    {
        const bool hasExplicitTarget =
            (desc.colorAttachmentCount > 0 && desc.colorAttachments[0].texture.valid()) ||
            desc.depthAttachment.texture.valid();
        if (!hasExplicitTarget)
        {
            // 附件为空 = 画到表面。GL 的「表面」就是 acquireNextImage 时
            // 捕获的当前帧缓冲（Qt 下是 widget 自己的 FBO，不是 0）。
            return m_surface ? m_surface->defaultFramebuffer() : 0;
        }
        return m_device->framebufferFor(desc);
    }

    RhiResult GlCommandList::beginRenderPass(const RenderPassBeginDesc& desc)
    {
        if (m_inRenderPass)
        {
            m_device->log().error("[gl] beginRenderPass: 上一个 RenderPass 未结束");
            return RhiResult::ErrorInvalidArgument;
        }
        if (desc.colorAttachmentCount > kMaxColorAttachments)
        {
            m_device->log().error("[gl] beginRenderPass: colorAttachmentCount=%u 超过上限 %u",
                                  desc.colorAttachmentCount, kMaxColorAttachments);
            return RhiResult::ErrorInvalidArgument;
        }

        const GLFuncs& f = m_device->gl();
        const GLuint fbo = resolveFramebuffer(desc);
        f.BindFramebuffer(GL_FRAMEBUFFER, fbo);

        m_passExtent = desc.extent;
        if (m_passExtent.width == 0 || m_passExtent.height == 0)
        {
            // 未给尺寸时退化为表面尺寸，避免视口为 0 导致「什么都没画」
            m_passExtent = m_surface ? m_surface->extent() : Extent2D{};
        }

        // 每个 Pass 开始时视口覆盖整个目标；调用方可随后 setViewport 覆盖。
        if (f.Viewport)
        {
            f.Viewport(0, 0, static_cast<GLsizei>(m_passExtent.width),
                       static_cast<GLsizei>(m_passExtent.height));
        }
        // 裁剪默认关闭：上一帧遗留的 scissor 会让本帧局部不刷新，
        // 这是 GL 全局状态最容易泄漏的一项。
        if (f.Disable)
        {
            f.Disable(GL_SCISSOR_TEST);
        }

        GLbitfield clearMask = 0;
        if (desc.colorAttachmentCount > 0 && desc.colorAttachments[0].loadOp == LoadOp::Clear)
        {
            const ColorRgba& c = desc.colorAttachments[0].clearValue;
            if (f.ClearColor)
            {
                f.ClearColor(c.r, c.g, c.b, c.a);
            }
            clearMask |= GL_COLOR_BUFFER_BIT;
        }
        const bool surfaceHasDepth = m_surface && m_surface->depthFormat() != Format::Unknown;
        const bool clearDepth = desc.depthAttachment.loadOp == LoadOp::Clear &&
                                (desc.hasDepthAttachment || surfaceHasDepth);
        if (clearDepth)
        {
            // GL 要求深度写开启才能清深度。管线可能把 depthWrite 关了，
            // 这里临时打开，清完由 applyPipelineState 重新按管线设定下发。
            if (f.DepthMask)
            {
                f.DepthMask(GL_TRUE);
            }
            clearMask |= GL_DEPTH_BUFFER_BIT;
        }
        if (clearMask != 0 && f.Clear)
        {
            f.Clear(clearMask);
        }

        m_inRenderPass = true;
        // 管线状态与 Pass 无关，但目标切换后需要重新下发一次，
        // 否则新目标沿用旧目标的 viewport/深度设置。
        m_pipelineHandle = PipelineHandle{};
        return RhiResult::Ok;
    }

    void GlCommandList::endRenderPass()
    {
        if (!m_inRenderPass)
        {
            m_device->log().error("[gl] endRenderPass: 当前不在 RenderPass 内");
            return;
        }
        m_inRenderPass = false;
    }

    void GlCommandList::applyPipelineState(const GlPipelineRecord& pipeline)
    {
        const GLFuncs& f = m_device->gl();

        f.UseProgram(pipeline.program);
        if (pipeline.vao && f.BindVertexArray)
        {
            f.BindVertexArray(pipeline.vao);
        }

        // ---- 深度 ----
        if (pipeline.depthStencil.depthTestEnable)
        {
            f.Enable(GL_DEPTH_TEST);
            f.DepthFunc(toGlCompareOp(pipeline.depthStencil.depthCompare));
        }
        else
        {
            f.Disable(GL_DEPTH_TEST);
        }
        if (f.DepthMask)
        {
            f.DepthMask(pipeline.depthStencil.depthWriteEnable ? GL_TRUE : GL_FALSE);
        }

        // ---- 混合 ----
        if (pipeline.blend.enable)
        {
            f.Enable(GL_BLEND);
            if (f.BlendFuncSeparate)
            {
                f.BlendFuncSeparate(toGlBlendFactor(pipeline.blend.srcColor),
                                    toGlBlendFactor(pipeline.blend.dstColor),
                                    toGlBlendFactor(pipeline.blend.srcAlpha),
                                    toGlBlendFactor(pipeline.blend.dstAlpha));
            }
            else if (f.BlendFunc)
            {
                // 退化路径：只能用颜色因子，alpha 因子被忽略。
                // 记一条 Debug 而不是静默，否则半透明表现不一致时无从查起。
                f.BlendFunc(toGlBlendFactor(pipeline.blend.srcColor),
                            toGlBlendFactor(pipeline.blend.dstColor));
                m_device->log().debug("[gl] 缺少 glBlendFuncSeparate，alpha 混合因子被忽略");
            }
            if (f.BlendEquationSeparate)
            {
                f.BlendEquationSeparate(toGlBlendOp(pipeline.blend.colorOp),
                                        toGlBlendOp(pipeline.blend.alphaOp));
            }
        }
        else
        {
            f.Disable(GL_BLEND);
        }

        // ---- 光栅化 ----
        if (pipeline.raster.cullMode == CullMode::None || !f.CullFace)
        {
            f.Disable(GL_CULL_FACE);
        }
        else
        {
            f.Enable(GL_CULL_FACE);
            f.CullFace(pipeline.raster.cullMode == CullMode::Front ? GL_FRONT : GL_BACK);
        }
        if (f.FrontFace)
        {
            f.FrontFace(pipeline.raster.frontFace == FrontFace::CounterClockwise ? GL_CCW : GL_CW);
        }
        if (f.PolygonMode)
        {
            f.PolygonMode(GL_FRONT_AND_BACK, pipeline.raster.fillMode == FillMode::Wireframe ? GL_LINE : GL_FILL);
        }
        if (f.LineWidth)
        {
            // 夹到设备实际支持的范围：超范围会产生 GL_INVALID_VALUE，
            // 而 GL 不会替你回退，后续状态全部作废。
            float width = pipeline.raster.lineWidth;
            if (width < 1.0f) { width = 1.0f; }
            if (width > m_device->capabilities().maxLineWidth) { width = m_device->capabilities().maxLineWidth; }
            f.LineWidth(width);
        }

        m_stats.pipelineSwitches += 1;
        // 顶点属性依附于本管线的 VAO，切管线后必须重挂缓冲
        m_vertexBindingsDirty = true;
        m_pushConstantsDirty = m_pushConstantsDirty || pipeline.pushConstantBytes > 0;
    }

    const GlPipelineRecord* GlCommandList::boundPipeline()
    {
        // 每次都重新解析，绝不缓存记录指针。
        //
        // 记录存放在 SlotMap 的稠密 std::vector 里，任何一次 createGraphicsPipeline()
        // 的 push_back 扩容都会把整块内存搬走 —— 此前缓存下来的指针立刻悬垂。
        // 管线是按 (顶点格式, 空间, 拓扑, 量化线宽) 懒建的，所以「录制途中新建管线」
        // 是常态而非例外：首帧画完第一条命令、给第二条解析新线宽的管线时就会发生。
        // 世代式句柄只能防「旧句柄命中新资源」，防不住调用方存了 get() 的返回值。
        return m_device ? m_device->pipelineRecord(m_pipelineHandle) : nullptr;
    }

    void GlCommandList::bindPipeline(PipelineHandle pipeline)
    {
        if (pipeline == m_pipelineHandle && boundPipeline())
        {
            return;
        }
        GlPipelineRecord* record = m_device->pipelineRecord(pipeline);
        if (!record)
        {
            m_device->log().error("[gl] bindPipeline: 管线句柄无效");
            return;
        }
        m_pipelineHandle = pipeline;
        applyPipelineState(*record);
    }

    void GlCommandList::setViewport(const Viewport& viewport)
    {
        const GLFuncs& f = m_device->gl();
        if (!f.Viewport)
        {
            return;
        }
        // RHI 的 y 轴向下（左上角原点），GL 向上，故做一次翻转。
        const int32_t flippedY =
            static_cast<int32_t>(m_passExtent.height) - static_cast<int32_t>(viewport.y + viewport.height);
        f.Viewport(static_cast<GLint>(viewport.x), static_cast<GLint>(flippedY),
                   static_cast<GLsizei>(viewport.width), static_cast<GLsizei>(viewport.height));
    }

    void GlCommandList::setScissor(const Rect2D& rect)
    {
        const GLFuncs& f = m_device->gl();
        if (!f.Scissor || !f.Enable)
        {
            return;
        }
        const int32_t flippedY =
            static_cast<int32_t>(m_passExtent.height) - (rect.y + static_cast<int32_t>(rect.height));
        f.Enable(GL_SCISSOR_TEST);
        f.Scissor(rect.x, flippedY, static_cast<GLsizei>(rect.width), static_cast<GLsizei>(rect.height));
    }

    void GlCommandList::bindVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offsetBytes)
    {
        if (slot >= kMaxVertexBufferSlots)
        {
            m_device->log().error("[gl] bindVertexBuffer: slot=%u 超过上限 %u", slot, kMaxVertexBufferSlots);
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

    void GlCommandList::bindIndexBuffer(BufferHandle buffer, uint64_t offsetBytes, IndexType type)
    {
        m_indexBuffer = buffer;
        m_indexOffset = offsetBytes;
        m_indexType = type;
    }

    void GlCommandList::flushVertexBindings()
    {
        if (!m_vertexBindingsDirty || !boundPipeline())
        {
            return;
        }
        const GLFuncs& f = m_device->gl();

        // 走经典 GL 3.3 路径（glVertexAttribPointer），不用 4.3 的
        // glBindVertexBuffer/glVertexAttribFormat：macOS 的 GL 上限是 4.1，
        // 用 4.3 入口会在 Mac 上直接拿到空指针。
        for (uint32_t i = 0; i < boundPipeline()->attributeCount; ++i)
        {
            const VertexAttribute& attr = boundPipeline()->attributes[i];
            if (attr.bufferSlot >= kMaxVertexBufferSlots)
            {
                continue;
            }
            const VertexBinding& binding = m_vertexBindings[attr.bufferSlot];
            GlBufferRecord* buffer = m_device->bufferRecord(binding.buffer);
            if (!buffer)
            {
                continue;
            }

            uint32_t stride = 0;
            bool perInstance = false;
            for (uint32_t s = 0; s < boundPipeline()->bufferLayoutCount; ++s)
            {
                if (boundPipeline()->bufferLayouts[s].slot == attr.bufferSlot)
                {
                    stride = boundPipeline()->bufferLayouts[s].stride;
                    perInstance = boundPipeline()->bufferLayouts[s].perInstance;
                    break;
                }
            }

            const GlVertexAttribFormat format = toGlVertexAttrib(attr.type);
            const auto pointer =
                reinterpret_cast<const void*>(static_cast<uintptr_t>(binding.offset + attr.offset));

            f.BindBuffer(GL_ARRAY_BUFFER, buffer->name);
            f.EnableVertexAttribArray(attr.location);
            if (format.isInteger && f.VertexAttribIPointer)
            {
                f.VertexAttribIPointer(attr.location, format.components, format.type,
                                       static_cast<GLsizei>(stride), pointer);
            }
            else
            {
                f.VertexAttribPointer(attr.location, format.components, format.type, format.normalized,
                                      static_cast<GLsizei>(stride), pointer);
            }
            if (f.VertexAttribDivisor)
            {
                f.VertexAttribDivisor(attr.location, perInstance ? 1 : 0);
            }
            else if (perInstance)
            {
                m_device->log().warn("[gl] 缺少 glVertexAttribDivisor，逐实例属性 location=%u "
                                     "会被当作逐顶点属性，实例化绘制结果不正确",
                                     attr.location);
            }
        }

        if (m_indexBuffer.valid())
        {
            GlBufferRecord* index = m_device->bufferRecord(m_indexBuffer);
            if (index)
            {
                // 索引缓冲绑定记录在 VAO 里，必须在 VAO 绑定之后设置
                f.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, index->name);
            }
        }

        m_vertexBindingsDirty = false;
    }

    void GlCommandList::bindBindGroup(uint32_t set, BindGroupHandle group)
    {
        if (set >= kMaxDescriptorSets)
        {
            m_device->log().error("[gl] bindBindGroup: set=%u 超过上限 %u", set, kMaxDescriptorSets);
            return;
        }
        if (!boundPipeline())
        {
            m_device->log().error("[gl] bindBindGroup 必须在 bindPipeline 之后调用："
                                  "(set,binding) → GL 槽位的映射保存在管线里");
            return;
        }
        GlBindGroupRecord* record = m_device->bindGroupRecord(group);
        if (!record)
        {
            m_device->log().error("[gl] bindBindGroup: 绑定组句柄无效");
            return;
        }

        const GLFuncs& f = m_device->gl();

        auto findSlot = [this, set](uint32_t binding, uint32_t* outSlot, BindingType* outType) -> bool {
            for (const GlBindingMapping& mapping : boundPipeline()->bindings)
            {
                if (mapping.set == set && mapping.binding == binding)
                {
                    *outSlot = mapping.glSlot;
                    *outType = mapping.type;
                    return true;
                }
            }
            return false;
        };

        for (const BufferBinding& entry : record->buffers)
        {
            uint32_t slot = 0;
            BindingType type = BindingType::UniformBuffer;
            if (!findSlot(entry.binding, &slot, &type))
            {
                m_device->log().warn("[gl] bindBindGroup: 当前管线未声明 set=%u binding=%u，已跳过", set,
                                     entry.binding);
                continue;
            }
            GlBufferRecord* buffer = m_device->bufferRecord(entry.buffer);
            if (!buffer || !f.BindBufferRange)
            {
                continue;
            }
            const GLenum target =
                type == BindingType::StorageBuffer ? GL_SHADER_STORAGE_BUFFER : GL_UNIFORM_BUFFER;
            const uint64_t size = entry.size == 0 ? buffer->desc.size - entry.offset : entry.size;
            f.BindBufferRange(target, slot, buffer->name, static_cast<GLintptr>(entry.offset),
                              static_cast<GLsizeiptr>(size));
        }

        for (const TextureBinding& entry : record->textures)
        {
            uint32_t unit = 0;
            BindingType type = BindingType::SampledTexture;
            if (!findSlot(entry.binding, &unit, &type))
            {
                m_device->log().warn("[gl] bindBindGroup: 当前管线未声明纹理 set=%u binding=%u，已跳过",
                                     set, entry.binding);
                continue;
            }
            GlTextureRecord* texture = m_device->textureRecord(entry.texture);
            if (!texture || !f.ActiveTexture)
            {
                continue;
            }
            f.ActiveTexture(GL_TEXTURE0 + unit);
            f.BindTexture(GL_TEXTURE_2D, texture->name);

            // 采样参数设在纹理上（本后端未使用 GL 采样器对象，见 createSampler 注释）
            if (GlSamplerRecord* sampler = m_device->samplerRecord(entry.sampler))
            {
                f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, toGlFilter(sampler->desc.minFilter));
                f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, toGlFilter(sampler->desc.magFilter));
                f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, toGlAddressMode(sampler->desc.addressU));
                f.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, toGlAddressMode(sampler->desc.addressV));
            }
        }

        m_stats.bindGroupSwitches += 1;
    }

    void GlCommandList::pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data)
    {
        if (!data || sizeBytes == 0)
        {
            return;
        }
        if (offsetBytes + sizeBytes > kMaxPushConstantBytes)
        {
            m_device->log().error("[gl] pushConstants: offset=%u size=%u 超过 kMaxPushConstantBytes=%u",
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

    void GlCommandList::flushPushConstants()
    {
        if (!m_pushConstantsDirty || !boundPipeline() || boundPipeline()->pushConstantBytes == 0)
        {
            return;
        }
        const GLFuncs& f = m_device->gl();
        const GLuint ubo = m_device->pushConstantUbo();
        if (!ubo || !f.BindBuffer || !f.BufferSubData || !f.BindBufferRange)
        {
            return;
        }

        const uint32_t bytes =
            m_pushConstantHighWater > boundPipeline()->pushConstantBytes ? boundPipeline()->pushConstantBytes
                                                                   : m_pushConstantHighWater;
        f.BindBuffer(GL_UNIFORM_BUFFER, ubo);
        f.BufferSubData(GL_UNIFORM_BUFFER, 0, static_cast<GLsizeiptr>(bytes), m_pushConstants);
        f.BindBuffer(GL_UNIFORM_BUFFER, 0);
        f.BindBufferRange(GL_UNIFORM_BUFFER, GlDevice::kPushConstantBinding, ubo, 0,
                          static_cast<GLsizeiptr>(boundPipeline()->pushConstantBytes));
        m_pushConstantsDirty = false;
    }

    bool GlCommandList::prepareDraw(const char* what)
    {
        if (!m_inRenderPass)
        {
            m_device->log().error("[gl] %s 必须在 beginRenderPass / endRenderPass 之间调用", what);
            return false;
        }
        if (!boundPipeline())
        {
            m_device->log().error("[gl] %s 之前必须先 bindPipeline", what);
            return false;
        }
        flushVertexBindings();
        flushPushConstants();
        return true;
    }

    void GlCommandList::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                             uint32_t firstInstance)
    {
        if (vertexCount == 0 || !prepareDraw("draw"))
        {
            return;
        }
        const GLFuncs& f = m_device->gl();
        if (instanceCount <= 1)
        {
            f.DrawArrays(boundPipeline()->topology, static_cast<GLint>(firstVertex),
                         static_cast<GLsizei>(vertexCount));
        }
        else if (f.DrawArraysInstanced)
        {
            if (firstInstance != 0)
            {
                // glDrawArraysInstancedBaseInstance（GL 4.2）未纳入函数表
                m_device->log().warn("[gl] draw: firstInstance=%u 不支持，已按 0 处理", firstInstance);
            }
            f.DrawArraysInstanced(boundPipeline()->topology, static_cast<GLint>(firstVertex),
                                  static_cast<GLsizei>(vertexCount), static_cast<GLsizei>(instanceCount));
        }
        else
        {
            m_device->log().error("[gl] draw: 缺少 glDrawArraysInstanced，实例化绘制被跳过");
            return;
        }
        m_stats.drawCalls += 1;
    }

    void GlCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                                    int32_t vertexOffset, uint32_t firstInstance)
    {
        if (indexCount == 0 || !prepareDraw("drawIndexed"))
        {
            return;
        }
        if (!m_indexBuffer.valid())
        {
            m_device->log().error("[gl] drawIndexed: 未绑定索引缓冲");
            return;
        }
        if (vertexOffset != 0)
        {
            // glDrawElementsBaseVertex 未纳入函数表；Capabilities::baseVertexOffset
            // 已声明为 false，上层不应走到这里。
            m_device->log().error("[gl] drawIndexed: 不支持 vertexOffset（baseVertexOffset=false）");
            return;
        }

        const GLenum type = m_indexType == IndexType::Uint16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        const uint32_t indexSize = m_indexType == IndexType::Uint16 ? 2u : 4u;
        const auto offset = reinterpret_cast<const void*>(
            static_cast<uintptr_t>(m_indexOffset + static_cast<uint64_t>(firstIndex) * indexSize));

        const GLFuncs& f = m_device->gl();
        if (instanceCount <= 1)
        {
            f.DrawElements(boundPipeline()->topology, static_cast<GLsizei>(indexCount), type, offset);
        }
        else if (f.DrawElementsInstanced)
        {
            if (firstInstance != 0)
            {
                m_device->log().warn("[gl] drawIndexed: firstInstance=%u 不支持，已按 0 处理", firstInstance);
            }
            f.DrawElementsInstanced(boundPipeline()->topology, static_cast<GLsizei>(indexCount), type, offset,
                                    static_cast<GLsizei>(instanceCount));
        }
        else
        {
            m_device->log().error("[gl] drawIndexed: 缺少 glDrawElementsInstanced，实例化绘制被跳过");
            return;
        }
        m_stats.drawCalls += 1;
    }

    void GlCommandList::drawIndirect(BufferHandle argsBuffer, uint64_t offsetBytes, uint32_t drawCount,
                                     uint32_t strideBytes)
    {
        if (!prepareDraw("drawIndirect"))
        {
            return;
        }
        const GLFuncs& f = m_device->gl();
        GlBufferRecord* args = m_device->bufferRecord(argsBuffer);
        if (!args || !f.DrawArraysIndirect)
        {
            m_device->log().error("[gl] drawIndirect 不可用（句柄无效或缺少 glDrawArraysIndirect）");
            return;
        }
        f.BindBuffer(GL_DRAW_INDIRECT_BUFFER, args->name);
        const auto indirect = reinterpret_cast<const void*>(static_cast<uintptr_t>(offsetBytes));
        if (drawCount > 1 && f.MultiDrawArraysIndirect)
        {
            f.MultiDrawArraysIndirect(boundPipeline()->topology, indirect, static_cast<GLsizei>(drawCount),
                                      static_cast<GLsizei>(strideBytes));
        }
        else
        {
            f.DrawArraysIndirect(boundPipeline()->topology, indirect);
            if (drawCount > 1)
            {
                m_device->log().warn("[gl] drawIndirect: 缺少 glMultiDrawArraysIndirect，只发出了 1 次绘制");
            }
        }
        m_stats.drawCalls += drawCount;
    }

    void GlCommandList::drawIndexedIndirect(BufferHandle argsBuffer, uint64_t offsetBytes,
                                            uint32_t drawCount, uint32_t strideBytes)
    {
        if (!prepareDraw("drawIndexedIndirect"))
        {
            return;
        }
        if (!m_indexBuffer.valid())
        {
            m_device->log().error("[gl] drawIndexedIndirect: 未绑定索引缓冲");
            return;
        }
        const GLFuncs& f = m_device->gl();
        GlBufferRecord* args = m_device->bufferRecord(argsBuffer);
        if (!args || !f.DrawElementsIndirect)
        {
            m_device->log().error("[gl] drawIndexedIndirect 不可用");
            return;
        }
        const GLenum type = m_indexType == IndexType::Uint16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT;
        f.BindBuffer(GL_DRAW_INDIRECT_BUFFER, args->name);
        const auto indirect = reinterpret_cast<const void*>(static_cast<uintptr_t>(offsetBytes));
        if (drawCount > 1 && f.MultiDrawElementsIndirect)
        {
            f.MultiDrawElementsIndirect(boundPipeline()->topology, type, indirect,
                                        static_cast<GLsizei>(drawCount), static_cast<GLsizei>(strideBytes));
        }
        else
        {
            f.DrawElementsIndirect(boundPipeline()->topology, type, indirect);
            if (drawCount > 1)
            {
                m_device->log().warn(
                    "[gl] drawIndexedIndirect: 缺少 glMultiDrawElementsIndirect，只发出了 1 次绘制");
            }
        }
        m_stats.drawCalls += drawCount;
    }

    void GlCommandList::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        if (m_inRenderPass)
        {
            m_device->log().error("[gl] dispatchCompute 必须在 RenderPass 之外调用");
            return;
        }
        if (!boundPipeline() || !boundPipeline()->isCompute)
        {
            m_device->log().error("[gl] dispatchCompute: 当前绑定的不是计算管线");
            return;
        }
        if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
        {
            return;
        }
        const GLFuncs& f = m_device->gl();
        if (!f.DispatchCompute)
        {
            m_device->log().error("[gl] dispatchCompute: 当前上下文不支持计算着色器");
            return;
        }
        flushPushConstants();
        f.DispatchCompute(groupsX, groupsY, groupsZ);
        m_stats.computeDispatches += 1;
    }

    void GlCommandList::barrier(BarrierScope before, BarrierScope after)
    {
        const GLFuncs& f = m_device->gl();
        if (!f.MemoryBarrier)
        {
            return;
        }
        // GL 的 glMemoryBarrier 只表达「之后的访问要看到之前的写入」，
        // 没有 src/dst 之分，因此两个 scope 合并成一次调用。
        const GLbitfield bits = toGlBarrierBits(before) | toGlBarrierBits(after);
        if (bits != 0)
        {
            f.MemoryBarrier(bits);
        }
    }

    void GlCommandList::copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst,
                                   uint64_t dstOffset, uint64_t sizeBytes)
    {
        const GLFuncs& f = m_device->gl();
        GlBufferRecord* source = m_device->bufferRecord(src);
        GlBufferRecord* target = m_device->bufferRecord(dst);
        if (!source || !target || sizeBytes == 0 || !f.CopyBufferSubData)
        {
            m_device->log().error("[gl] copyBuffer 不可用（句柄无效或缺少 glCopyBufferSubData）");
            return;
        }
        // 用专用的 COPY_READ/COPY_WRITE 绑定点，避免污染 ARRAY_BUFFER 等
        // 正在使用的绑定点——这是 GL 全局状态最容易互相踩的地方。
        f.BindBuffer(GL_COPY_READ_BUFFER, source->name);
        f.BindBuffer(GL_COPY_WRITE_BUFFER, target->name);
        f.CopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, static_cast<GLintptr>(srcOffset),
                            static_cast<GLintptr>(dstOffset), static_cast<GLsizeiptr>(sizeBytes));
        f.BindBuffer(GL_COPY_READ_BUFFER, 0);
        f.BindBuffer(GL_COPY_WRITE_BUFFER, 0);
    }

    void GlCommandList::copyTextureToBuffer(TextureHandle src, BufferHandle dst, uint64_t dstOffset,
                                            const Rect2D& region)
    {
        if (m_inRenderPass)
        {
            m_device->log().error("[gl] copyTextureToBuffer 必须在 RenderPass 之外调用");
            return;
        }
        const GLFuncs& f = m_device->gl();
        GlTextureRecord* texture = m_device->textureRecord(src);
        GlBufferRecord* buffer = m_device->bufferRecord(dst);
        if (!texture || !buffer || !f.ReadPixels || !f.GenFramebuffers)
        {
            m_device->log().error("[gl] copyTextureToBuffer 不可用");
            return;
        }

        RenderPassBeginDesc desc{};
        desc.colorAttachmentCount = 1;
        desc.colorAttachments[0].texture = src;
        desc.colorAttachments[0].loadOp = LoadOp::Load;
        const GLuint fbo = m_device->framebufferFor(desc);
        if (!fbo)
        {
            return;
        }

        GLint previousFbo = 0;
        if (f.GetIntegerv)
        {
            f.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFbo);
        }
        f.BindFramebuffer(GL_FRAMEBUFFER, fbo);
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, buffer->name);
        if (f.PixelStorei)
        {
            f.PixelStorei(GL_PACK_ALIGNMENT, 1);
        }
        // 注意：走 PBO 时 ReadPixels 的最后一个参数是缓冲内偏移，不是指针。
        // 这里不做 y 翻转——数据落在 GPU 缓冲里由 shader 使用，
        // 由使用方按 GL 的左下角原点解释；需要 CPU 侧左上角原点时用 readTexture。
        f.ReadPixels(region.x, region.y, static_cast<GLsizei>(region.width),
                     static_cast<GLsizei>(region.height), toGlBaseFormat(texture->desc.format),
                     toGlPixelType(texture->desc.format),
                     reinterpret_cast<void*>(static_cast<uintptr_t>(dstOffset)));
        f.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        f.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo < 0 ? 0 : previousFbo));
    }

    void GlCommandList::pushDebugGroup(const char* name)
    {
        // glPushDebugGroup（KHR_debug）未纳入函数表；保留接口以便三个后端
        // 语义一致，Metal/Vulkan 会实现为真正的调试标记。
        (void)name;
    }

    void GlCommandList::popDebugGroup() {}

}  // namespace Render::RHI::gl
