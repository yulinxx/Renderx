/**
 * @file nullBackend.cpp
 * @brief Null 后端：新 RHI 的完整语义实现，不触碰任何 GPU
 *
 * 用途有三个，都不是「占位」：
 *
 * 1. 单元测试与 CI。没有 GPU/窗口系统也能验证句柄生命周期、帧配对、
 *    RenderPass 嵌套、pushConstant 越界、统计计数这些与后端无关的契约。
 * 2. 契约参考实现。三个真实后端必须与本文件的可观测行为一致
 *    （同样的调用序列 → 同样的 RhiResult），这是「上层代码在三个后端上
 *    语义完全一致」的判定基准。
 * 3. 无头渲染的降级目标——但**必须由调用方显式选择** BackendKind::Null。
 *    旧实现会在 Vulkan/Metal 不可用时静默换成 Null，结果是画面全黑
 *    而调用方拿不到任何错误；createDevice 现在不做这种回退。
 *
 * 与旧 rhiNull.cpp 的差异：本实现真实分配 CPU 侧存储，
 * writeBuffer/mapBuffer/writeTexture/readTexture 的数据可回读，
 * 因此能测出偏移与行距计算错误；旧实现全部是空函数，测不出任何东西。
 */

#include "rhi/rhiBackendFactory.h"
#include "rhi/rhiLog.h"
#include "rhi/rhiResourcePool.h"

#include <cstring>
#include <vector>

namespace Render::RHI
{
    namespace
    {
        // ==================== 资源记录 ====================

        struct NullBufferRecord
        {
            BufferDesc desc{};
            std::vector<uint8_t> storage;
            bool mapped = false;
            uint64_t mappedOffset = 0;
            uint64_t mappedSize = 0;
        };

        struct NullTextureRecord
        {
            TextureDesc desc{};
            std::vector<uint8_t> storage;
            uint32_t rowPitch = 0;
        };

        struct NullShaderRecord
        {
            ShaderLanguage language = ShaderLanguage::GlslSource;
            uint64_t sizeBytes = 0;
        };

        struct NullPipelineRecord
        {
            bool isCompute = false;
            uint32_t pushConstantBytes = 0;
            uint32_t attributeCount = 0;
        };

        struct NullSamplerRecord
        {
            SamplerDesc desc{};
        };

        struct NullBindGroupRecord
        {
            uint32_t bufferCount = 0;
            uint32_t textureCount = 0;
        };

        // ==================== 命令记录器 ====================

        /**
         * @brief Null 命令记录器
         *
         * 不产生任何 GPU 工作，但完整校验调用契约并累计统计。
         * 违约（未 begin 就 draw、RenderPass 嵌套、pushConstant 越界）
         * 记 Error 日志并忽略该次调用——与真实后端的表现一致：
         * RHI 不抛异常，也不因为上层用错而崩掉整个进程。
         */
        class NullCommandList final : public ICommandList
        {
        public:
            explicit NullCommandList(const RhiLogger& logger) : m_log(logger) {}

            void beginFrame()
            {
                m_stats = FrameStats{};
                m_inRenderPass = false;
                m_boundPipeline = PipelineHandle{};
            }

            bool inRenderPass() const { return m_inRenderPass; }

            RhiResult beginRenderPass(const RenderPassBeginDesc& desc) override
            {
                if (m_inRenderPass)
                {
                    m_log.error("[null] beginRenderPass: 上一个 RenderPass 未结束");
                    return RhiResult::ErrorInvalidArgument;
                }
                if (desc.colorAttachmentCount > kMaxColorAttachments)
                {
                    m_log.error("[null] beginRenderPass: colorAttachmentCount=%u 超过上限 %u",
                                desc.colorAttachmentCount, kMaxColorAttachments);
                    return RhiResult::ErrorInvalidArgument;
                }
                m_inRenderPass = true;
                return RhiResult::Ok;
            }

            void endRenderPass() override
            {
                if (!m_inRenderPass)
                {
                    m_log.error("[null] endRenderPass: 当前不在 RenderPass 内");
                    return;
                }
                m_inRenderPass = false;
            }

            void bindPipeline(PipelineHandle pipeline) override
            {
                if (pipeline != m_boundPipeline)
                {
                    m_boundPipeline = pipeline;
                    m_stats.pipelineSwitches += 1;
                }
            }

            void setViewport(const Viewport&) override {}
            void setScissor(const Rect2D&) override {}

            void bindVertexBuffer(uint32_t, BufferHandle, uint64_t) override {}
            void bindIndexBuffer(BufferHandle, uint64_t, IndexType) override {}

            void bindBindGroup(uint32_t set, BindGroupHandle) override
            {
                if (set >= kMaxDescriptorSets)
                {
                    m_log.error("[null] bindBindGroup: set=%u 超过上限 %u", set, kMaxDescriptorSets);
                    return;
                }
                m_stats.bindGroupSwitches += 1;
            }

            void pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data) override
            {
                if (!data || sizeBytes == 0)
                {
                    return;
                }
                if (offsetBytes + sizeBytes > kMaxPushConstantBytes)
                {
                    m_log.error("[null] pushConstants: offset=%u size=%u 超过 kMaxPushConstantBytes=%u",
                                offsetBytes, sizeBytes, kMaxPushConstantBytes);
                    return;
                }
                std::memcpy(m_pushConstants + offsetBytes, data, sizeBytes);
            }

            void draw(uint32_t vertexCount, uint32_t, uint32_t, uint32_t) override
            {
                if (!requireRenderPass("draw") || vertexCount == 0)
                {
                    return;
                }
                m_stats.drawCalls += 1;
            }

            void drawIndexed(uint32_t indexCount, uint32_t, uint32_t, int32_t, uint32_t) override
            {
                if (!requireRenderPass("drawIndexed") || indexCount == 0)
                {
                    return;
                }
                m_stats.drawCalls += 1;
            }

            void drawIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t) override
            {
                if (!requireRenderPass("drawIndirect"))
                {
                    return;
                }
                m_stats.drawCalls += drawCount;
            }

            void drawIndexedIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t) override
            {
                if (!requireRenderPass("drawIndexedIndirect"))
                {
                    return;
                }
                m_stats.drawCalls += drawCount;
            }

            void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override
            {
                if (m_inRenderPass)
                {
                    m_log.error("[null] dispatchCompute 必须在 RenderPass 之外调用");
                    return;
                }
                if (groupsX == 0 || groupsY == 0 || groupsZ == 0)
                {
                    return;
                }
                m_stats.computeDispatches += 1;
            }

            void barrier(BarrierScope, BarrierScope) override {}

            void copyBuffer(BufferHandle, uint64_t, BufferHandle, uint64_t, uint64_t) override {}

            void copyTextureToBuffer(TextureHandle, BufferHandle, uint64_t, const Rect2D&) override
            {
                if (m_inRenderPass)
                {
                    m_log.error("[null] copyTextureToBuffer 必须在 RenderPass 之外调用");
                }
            }

            void pushDebugGroup(const char*) override {}
            void popDebugGroup() override {}

            FrameStats stats() const override { return m_stats; }

            void addVertexBytes(uint64_t bytes) { m_stats.vertexBytesUploaded += bytes; }

        private:
            bool requireRenderPass(const char* what)
            {
                if (!m_inRenderPass)
                {
                    m_log.error("[null] %s 必须在 beginRenderPass / endRenderPass 之间调用", what);
                    return false;
                }
                return true;
            }

            RhiLogger m_log;
            FrameStats m_stats{};
            bool m_inRenderPass = false;
            PipelineHandle m_boundPipeline{};
            uint8_t m_pushConstants[kMaxPushConstantBytes]{};
        };

        class NullDevice;

        // ==================== 表面 ====================

        /**
         * @brief Null 表面
         *
         * 颜色/深度附件是设备上真实分配的纹理记录，因此
         * currentColorTexture() 返回的是可用于 RenderPassBeginDesc 与
         * readTexture 的有效句柄——测试可以据此验证附件尺寸随 resize 变化。
         */
        class NullSurface final : public ISurface
        {
        public:
            NullSurface(NullDevice* device, const SurfaceDesc& desc, const RhiLogger& logger)
                : m_device(device), m_desc(desc), m_extent(desc.initialExtent), m_log(logger)
            {
            }

            RhiResult acquireNextImage() override
            {
                if (m_extent.width == 0 || m_extent.height == 0)
                {
                    // 窗口最小化：不是错误，但本帧不应渲染
                    return RhiResult::ErrorSwapchainOutOfDate;
                }
                m_acquired = true;
                return RhiResult::Ok;
            }

            RhiResult present() override
            {
                if (!m_acquired)
                {
                    m_log.error("[null] present: 未先调用 acquireNextImage");
                    return RhiResult::ErrorInvalidArgument;
                }
                m_acquired = false;
                m_presentCount += 1;
                return RhiResult::Ok;
            }

            RhiResult resize(Extent2D extent) override;

            Extent2D extent() const override { return m_extent; }
            Format colorFormat() const override { return m_desc.preferredColorFormat; }
            Format depthFormat() const override { return m_desc.depthFormat; }
            TextureHandle currentColorTexture() const override { return m_colorTexture; }
            TextureHandle depthTexture() const override { return m_depthTexture; }

            uint64_t presentCount() const { return m_presentCount; }

            void setAttachments(TextureHandle color, TextureHandle depth)
            {
                m_colorTexture = color;
                m_depthTexture = depth;
            }
            TextureHandle colorAttachment() const { return m_colorTexture; }
            TextureHandle depthAttachment() const { return m_depthTexture; }
            const SurfaceDesc& desc() const { return m_desc; }
            void setExtent(Extent2D e) { m_extent = e; }

        private:
            NullDevice* m_device = nullptr;
            SurfaceDesc m_desc{};
            Extent2D m_extent{};
            TextureHandle m_colorTexture{};
            TextureHandle m_depthTexture{};
            bool m_acquired = false;
            uint64_t m_presentCount = 0;
            RhiLogger m_log;
        };

        // ==================== 设备 ====================

        class NullDevice final : public IGpuDevice
        {
        public:
            explicit NullDevice(const DeviceDesc& desc)
                : m_log(desc.logCallback, desc.logUserData), m_commands(m_log)
            {
                m_caps.backend = BackendKind::Null;
                m_caps.acceptedShaderLanguage = ShaderLanguage::GlslSource;
                std::snprintf(m_caps.deviceName, sizeof(m_caps.deviceName), "Null Device");
                std::snprintf(m_caps.driverInfo, sizeof(m_caps.driverInfo), "RenderX null backend");
                // Null 后端声明支持全部可选特性：它的作用是让上层代码路径
                // 都能在无 GPU 环境下被测到，而不是让上层走降级分支。
                m_caps.computeShaders = true;
                m_caps.indirectDraw = true;
                m_caps.multiDrawIndirect = true;
                m_caps.storageBuffers = true;
                m_caps.wireframeFill = true;
                m_caps.baseVertexOffset = true;
                m_caps.persistentMapping = true;
                m_caps.timestampQueries = false;
                m_caps.maxLineWidth = 1.0f;
                m_caps.maxTextureSize = 8192;
                m_caps.maxColorAttachments = kMaxColorAttachments;
                m_caps.maxPushConstantBytes = kMaxPushConstantBytes;
                m_caps.uniformBufferOffsetAlignment = 256;
                m_caps.storageBufferOffsetAlignment = 256;
                m_caps.maxFramesInFlight = 2;

                m_log.info("[null] 设备已创建");
            }

            ~NullDevice() override
            {
                if (!m_surfaces.empty())
                {
                    // 契约是「所有 ISurface 必须先销毁」。这里不静默清理，
                    // 因为静默清理会掩盖宿主的生命周期错误（旧实现的
                    // Runtime::destroy 双重释放就是这么被掩盖了半年）。
                    m_log.error("[null] 设备销毁时仍有 %zu 个表面未销毁", m_surfaces.size());
                    for (NullSurface* s : m_surfaces)
                    {
                        delete s;
                    }
                    m_surfaces.clear();
                }
                m_log.info("[null] 设备已销毁");
            }

            const Capabilities& capabilities() const override { return m_caps; }

            // ---------- 表面 ----------

            ISurface* createSurface(const SurfaceDesc& desc) override
            {
                auto* surface = new NullSurface(this, desc, m_log);
                surface->setAttachments(createSurfaceAttachment(desc.preferredColorFormat, desc.initialExtent,
                                                                TextureUsage::ColorAttachment),
                                        desc.depthFormat == Format::Unknown
                                            ? TextureHandle{}
                                            : createSurfaceAttachment(desc.depthFormat, desc.initialExtent,
                                                                      TextureUsage::DepthStencilAttachment));
                m_surfaces.push_back(surface);
                m_log.debug("[null] createSurface: %ux%u（当前表面数 %zu）", desc.initialExtent.width,
                            desc.initialExtent.height, m_surfaces.size());
                return surface;
            }

            void destroySurface(ISurface* surface) override
            {
                if (!surface)
                {
                    return;
                }
                for (size_t i = 0; i < m_surfaces.size(); ++i)
                {
                    if (m_surfaces[i] != surface)
                    {
                        continue;
                    }
                    auto* s = m_surfaces[i];
                    destroyTexture(s->colorAttachment());
                    destroyTexture(s->depthAttachment());
                    m_surfaces[i] = m_surfaces.back();
                    m_surfaces.pop_back();
                    delete s;
                    return;
                }
                m_log.error("[null] destroySurface: 表面不属于本设备");
            }

            TextureHandle createSurfaceAttachment(Format format, Extent2D extent, TextureUsage usage)
            {
                TextureDesc td{};
                td.width = extent.width == 0 ? 1 : extent.width;
                td.height = extent.height == 0 ? 1 : extent.height;
                td.format = format;
                td.usage = usage;
                td.debugName = "NullSurfaceAttachment";
                return createTexture(td);
            }

            // ---------- 着色器 ----------

            ShaderHandle createShader(const ShaderDesc& desc) override
            {
                if (!desc.data || desc.sizeBytes == 0)
                {
                    m_log.error("[null] createShader: 空字节码");
                    return ShaderHandle{};
                }
                if (desc.language != m_caps.acceptedShaderLanguage)
                {
                    m_log.error("[null] createShader: 语言 %d 与后端接受的 %d 不匹配",
                                static_cast<int>(desc.language),
                                static_cast<int>(m_caps.acceptedShaderLanguage));
                    return ShaderHandle{};
                }
                return m_shaders.add(NullShaderRecord{ desc.language, desc.sizeBytes });
            }

            void destroyShader(ShaderHandle shader) override
            {
                if (shader.valid() && !m_shaders.remove(shader))
                {
                    m_log.warn("[null] destroyShader: 句柄已失效（重复销毁？）");
                }
            }

            // ---------- 管线 ----------

            PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override
            {
                if (!m_shaders.get(desc.vertexShader) || !m_shaders.get(desc.fragmentShader))
                {
                    m_log.error("[null] createGraphicsPipeline: 顶点或片段着色器句柄无效");
                    return PipelineHandle{};
                }
                if (desc.attributeCount > kMaxVertexAttributes)
                {
                    m_log.error("[null] createGraphicsPipeline: attributeCount=%u 超过上限 %u",
                                desc.attributeCount, kMaxVertexAttributes);
                    return PipelineHandle{};
                }
                if (desc.pushConstantBytes > kMaxPushConstantBytes)
                {
                    m_log.error("[null] createGraphicsPipeline: pushConstantBytes=%u 超过上限 %u",
                                desc.pushConstantBytes, kMaxPushConstantBytes);
                    return PipelineHandle{};
                }
                return m_pipelines.add(
                    NullPipelineRecord{ false, desc.pushConstantBytes, desc.attributeCount });
            }

            PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override
            {
                if (!m_shaders.get(desc.computeShader))
                {
                    m_log.error("[null] createComputePipeline: 计算着色器句柄无效");
                    return PipelineHandle{};
                }
                return m_pipelines.add(NullPipelineRecord{ true, desc.pushConstantBytes, 0 });
            }

            void destroyPipeline(PipelineHandle pipeline) override
            {
                if (pipeline.valid() && !m_pipelines.remove(pipeline))
                {
                    m_log.warn("[null] destroyPipeline: 句柄已失效（重复销毁？）");
                }
            }

            // ---------- 缓冲区 ----------

            BufferHandle createBuffer(const BufferDesc& desc) override
            {
                if (desc.size == 0)
                {
                    m_log.error("[null] createBuffer: size 为 0");
                    return BufferHandle{};
                }
                NullBufferRecord record{};
                record.desc = desc;
                record.storage.assign(static_cast<size_t>(desc.size), 0);
                return m_buffers.add(std::move(record));
            }

            void destroyBuffer(BufferHandle buffer) override
            {
                if (buffer.valid() && !m_buffers.remove(buffer))
                {
                    m_log.warn("[null] destroyBuffer: 句柄已失效（重复销毁？）");
                }
            }

            RhiResult writeBuffer(BufferHandle buffer, uint64_t offset, const void* data,
                                  uint64_t sizeBytes) override
            {
                auto* record = m_buffers.get(buffer);
                if (!record || !data)
                {
                    return RhiResult::ErrorInvalidArgument;
                }
                if (offset + sizeBytes > record->desc.size)
                {
                    m_log.error("[null] writeBuffer 越界: offset=%llu size=%llu 缓冲大小=%llu",
                                static_cast<unsigned long long>(offset),
                                static_cast<unsigned long long>(sizeBytes),
                                static_cast<unsigned long long>(record->desc.size));
                    return RhiResult::ErrorInvalidArgument;
                }
                std::memcpy(record->storage.data() + offset, data, static_cast<size_t>(sizeBytes));
                m_commands.addVertexBytes(sizeBytes);
                return RhiResult::Ok;
            }

            MappedRange mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes) override
            {
                auto* record = m_buffers.get(buffer);
                if (!record)
                {
                    return MappedRange{};
                }
                if (record->desc.access == MemoryAccess::GpuOnly)
                {
                    m_log.error("[null] mapBuffer: GpuOnly 缓冲不可映射");
                    return MappedRange{};
                }
                if (record->mapped)
                {
                    m_log.error("[null] mapBuffer: 该缓冲已处于映射状态");
                    return MappedRange{};
                }
                const uint64_t size = sizeBytes == 0 ? record->desc.size - offset : sizeBytes;
                if (offset + size > record->desc.size)
                {
                    m_log.error("[null] mapBuffer 越界");
                    return MappedRange{};
                }
                record->mapped = true;
                record->mappedOffset = offset;
                record->mappedSize = size;
                return MappedRange{ record->storage.data() + offset, offset, size };
            }

            void unmapBuffer(BufferHandle buffer) override
            {
                auto* record = m_buffers.get(buffer);
                if (!record)
                {
                    return;
                }
                if (!record->mapped)
                {
                    m_log.warn("[null] unmapBuffer: 该缓冲未处于映射状态");
                    return;
                }
                record->mapped = false;
            }

            void flushMappedRange(BufferHandle, uint64_t, uint64_t) override {}

            // ---------- 纹理与采样器 ----------

            TextureHandle createTexture(const TextureDesc& desc) override
            {
                if (desc.width == 0 || desc.height == 0)
                {
                    m_log.error("[null] createTexture: 尺寸为 0");
                    return TextureHandle{};
                }
                if (desc.width > m_caps.maxTextureSize || desc.height > m_caps.maxTextureSize)
                {
                    m_log.error("[null] createTexture: %ux%u 超过 maxTextureSize=%u", desc.width, desc.height,
                                m_caps.maxTextureSize);
                    return TextureHandle{};
                }
                const uint32_t pixelSize = formatByteSize(desc.format);
                if (pixelSize == 0)
                {
                    m_log.error("[null] createTexture: 未知格式");
                    return TextureHandle{};
                }
                NullTextureRecord record{};
                record.desc = desc;
                record.rowPitch = desc.width * pixelSize;
                record.storage.assign(static_cast<size_t>(record.rowPitch) * desc.height, 0);
                return m_textures.add(std::move(record));
            }

            void destroyTexture(TextureHandle texture) override
            {
                if (texture.valid() && !m_textures.remove(texture))
                {
                    m_log.warn("[null] destroyTexture: 句柄已失效（重复销毁？）");
                }
            }

            RhiResult writeTexture(TextureHandle texture, uint32_t mipLevel, const Rect2D& region,
                                   const void* data, uint64_t sizeBytes) override
            {
                auto* record = m_textures.get(texture);
                if (!record || !data)
                {
                    return RhiResult::ErrorInvalidArgument;
                }
                if (mipLevel != 0)
                {
                    // Null 后端只保存 mip 0 的数据；上层若依赖 mip 上传，
                    // 说明该路径需要在真实后端上验证。
                    return RhiResult::ErrorUnsupported;
                }
                if (region.x < 0 || region.y < 0 ||
                    static_cast<uint32_t>(region.x) + region.width > record->desc.width ||
                    static_cast<uint32_t>(region.y) + region.height > record->desc.height)
                {
                    m_log.error("[null] writeTexture: 区域超出纹理范围");
                    return RhiResult::ErrorInvalidArgument;
                }
                const uint32_t pixelSize = formatByteSize(record->desc.format);
                const uint64_t needed = static_cast<uint64_t>(region.width) * region.height * pixelSize;
                if (sizeBytes < needed)
                {
                    m_log.error("[null] writeTexture: 数据不足，需要 %llu 字节，给了 %llu",
                                static_cast<unsigned long long>(needed),
                                static_cast<unsigned long long>(sizeBytes));
                    return RhiResult::ErrorInvalidArgument;
                }
                const auto* src = static_cast<const uint8_t*>(data);
                for (uint32_t row = 0; row < region.height; ++row)
                {
                    uint8_t* dst = record->storage.data() +
                                   static_cast<size_t>(region.y + row) * record->rowPitch +
                                   static_cast<size_t>(region.x) * pixelSize;
                    std::memcpy(dst, src + static_cast<size_t>(row) * region.width * pixelSize,
                                static_cast<size_t>(region.width) * pixelSize);
                }
                return RhiResult::Ok;
            }

            SamplerHandle createSampler(const SamplerDesc& desc) override
            {
                return m_samplers.add(NullSamplerRecord{ desc });
            }

            void destroySampler(SamplerHandle sampler) override
            {
                if (sampler.valid() && !m_samplers.remove(sampler))
                {
                    m_log.warn("[null] destroySampler: 句柄已失效（重复销毁？）");
                }
            }

            // ---------- 绑定组 ----------

            BindGroupHandle createBindGroup(const BindGroupDesc& desc) override
            {
                for (uint32_t i = 0; i < desc.bufferCount; ++i)
                {
                    if (!m_buffers.get(desc.buffers[i].buffer))
                    {
                        m_log.error("[null] createBindGroup: buffers[%u] 句柄无效", i);
                        return BindGroupHandle{};
                    }
                }
                for (uint32_t i = 0; i < desc.textureCount; ++i)
                {
                    if (!m_textures.get(desc.textures[i].texture))
                    {
                        m_log.error("[null] createBindGroup: textures[%u] 句柄无效", i);
                        return BindGroupHandle{};
                    }
                }
                return m_bindGroups.add(NullBindGroupRecord{ desc.bufferCount, desc.textureCount });
            }

            void destroyBindGroup(BindGroupHandle group) override
            {
                if (group.valid() && !m_bindGroups.remove(group))
                {
                    m_log.warn("[null] destroyBindGroup: 句柄已失效（重复销毁？）");
                }
            }

            // ---------- 帧与提交 ----------

            ICommandList* beginFrame(ISurface* surface) override
            {
                if (!surface)
                {
                    m_log.error("[null] beginFrame: surface 为空");
                    return nullptr;
                }
                if (m_inFrame)
                {
                    m_log.error("[null] beginFrame: 上一帧未 submitFrame");
                    return nullptr;
                }
                m_inFrame = true;
                m_commands.beginFrame();
                return &m_commands;
            }

            RhiResult submitFrame() override
            {
                if (!m_inFrame)
                {
                    m_log.error("[null] submitFrame: 未先调用 beginFrame");
                    return RhiResult::ErrorInvalidArgument;
                }
                if (m_commands.inRenderPass())
                {
                    m_log.error("[null] submitFrame: RenderPass 未结束");
                    return RhiResult::ErrorInvalidArgument;
                }
                m_inFrame = false;
                m_frameIndex += 1;
                return RhiResult::Ok;
            }

            void waitIdle() override {}

            // ---------- 读回 ----------

            RhiResult readTexture(TextureHandle texture, const Rect2D& region, void* outPixels,
                                  uint64_t bufferSize, uint32_t* outRowPitch) override
            {
                auto* record = m_textures.get(texture);
                if (!record || !outPixels)
                {
                    return RhiResult::ErrorInvalidArgument;
                }
                if (region.x < 0 || region.y < 0 ||
                    static_cast<uint32_t>(region.x) + region.width > record->desc.width ||
                    static_cast<uint32_t>(region.y) + region.height > record->desc.height)
                {
                    return RhiResult::ErrorInvalidArgument;
                }
                const uint32_t pixelSize = formatByteSize(record->desc.format);
                const uint32_t rowPitch = region.width * pixelSize;
                if (bufferSize < static_cast<uint64_t>(rowPitch) * region.height)
                {
                    return RhiResult::ErrorInvalidArgument;
                }
                auto* dst = static_cast<uint8_t*>(outPixels);
                for (uint32_t row = 0; row < region.height; ++row)
                {
                    // 像素原点为左上角：第 0 行就是纹理第 region.y 行，
                    // 不做任何翻转（GL 后端负责在自己那侧翻转）。
                    const uint8_t* src = record->storage.data() +
                                         static_cast<size_t>(region.y + row) * record->rowPitch +
                                         static_cast<size_t>(region.x) * pixelSize;
                    std::memcpy(dst + static_cast<size_t>(row) * rowPitch, src, rowPitch);
                }
                if (outRowPitch)
                {
                    *outRowPitch = rowPitch;
                }
                return RhiResult::Ok;
            }

            uint64_t gpuMemoryUsageBytes() const override
            {
                uint64_t total = 0;
                for (const auto& buffer : const_cast<NullDevice*>(this)->m_buffers)
                {
                    total += buffer.desc.size;
                }
                for (const auto& texture : const_cast<NullDevice*>(this)->m_textures)
                {
                    total += texture.storage.size();
                }
                return total;
            }

            uint64_t frameIndex() const { return m_frameIndex; }

        private:
            RhiLogger m_log;
            Capabilities m_caps{};
            NullCommandList m_commands;
            bool m_inFrame = false;
            uint64_t m_frameIndex = 0;

            std::vector<NullSurface*> m_surfaces;
            ResourcePool<BufferHandle, NullBufferRecord> m_buffers;
            ResourcePool<TextureHandle, NullTextureRecord> m_textures;
            ResourcePool<SamplerHandle, NullSamplerRecord> m_samplers;
            ResourcePool<ShaderHandle, NullShaderRecord> m_shaders;
            ResourcePool<PipelineHandle, NullPipelineRecord> m_pipelines;
            ResourcePool<BindGroupHandle, NullBindGroupRecord> m_bindGroups;
        };

        RhiResult NullSurface::resize(Extent2D extent)
        {
            if (extent.width == 0 || extent.height == 0)
            {
                // 最小化：记录尺寸但不重建附件，acquireNextImage 会返回
                // ErrorSwapchainOutOfDate 让调用方跳过本帧
                m_extent = extent;
                return RhiResult::Ok;
            }
            if (extent.width == m_extent.width && extent.height == m_extent.height)
            {
                return RhiResult::Ok;
            }

            m_device->destroyTexture(m_colorTexture);
            m_device->destroyTexture(m_depthTexture);
            m_extent = extent;
            m_colorTexture = m_device->createSurfaceAttachment(m_desc.preferredColorFormat, extent,
                                                               TextureUsage::ColorAttachment);
            m_depthTexture = m_desc.depthFormat == Format::Unknown
                                 ? TextureHandle{}
                                 : m_device->createSurfaceAttachment(m_desc.depthFormat, extent,
                                                                     TextureUsage::DepthStencilAttachment);
            return RhiResult::Ok;
        }

    }  // namespace

    IGpuDevice* createNullDevice(const DeviceDesc& desc)
    {
        return new NullDevice(desc);
    }

}  // namespace Render::RHI
