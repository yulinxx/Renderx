/**
 * @file glDevice.h
 * @brief 新 RHI 的 OpenGL 后端：设备 / 表面 / 命令记录器 声明
 *
 * 与旧 rhiGl 的关键差异（每一条都对应一个已确认的缺陷）：
 *
 * 1. **每设备一份 GL 函数表**。旧实现用 glLoader.cpp 里的进程级 `g_funcs`，
 *    每次设备初始化都清零重解析。Windows 上 wglGetProcAddress 的返回值只对
 *    当前像素格式有效，第二个窗口初始化会覆盖第一个窗口的函数指针——
 *    多窗口下必坏。本实现把表放进 GlDevice。
 * 2. **表面/交换链是独立对象**。旧 IDevice 的 present() 是空函数，
 *    窗口句柄存了却从不读。GlSurface 显式持有窗口与默认帧缓冲。
 * 3. **命令记录模型**，不再是「立即改全局 GL 状态」。状态在 bindPipeline
 *    时按管线一次性下发，绘制前统一 flush 顶点绑定与 pushConstant。
 * 4. **(set, binding) 描述符 + pushConstant**，不再有按字符串查 uniform。
 *    字符串只在 createGraphicsPipeline 时用一次，用于把块名/采样器名解析成
 *    GL 的 binding point 与纹理单元。
 * 5. **读回统一左上角原点**。GL 的 ReadPixels 是左下角原点，翻转在本层完成，
 *    不把 GL 的坐标习惯泄漏给调用方。
 *
 * 线程契约（与 rhiSurface.h 一致）：一个 GlDevice 及其派生对象只能在
 * 绑定了对应 GL 上下文的线程上使用。RHI 自身不加锁。
 */
#pragma once

#include "rhi/gl/glCommon.h"
#include "rhi/rhiCommandList.h"
#include "rhi/rhiGpuDevice.h"
#include "rhi/rhiLog.h"
#include "rhi/rhiResourcePool.h"
#include "rhi/rhiSurface.h"

#include <vector>

namespace Render::RHI::gl
{

    class GlDevice;

    /**
     * @brief GL 表面
     *
     * 当前只支持 NativeWindow::Kind::ForeignGlContext：上下文由宿主
     * （Qt 的 QOpenGLWidget 等）创建并保证在调用 RHI 期间为当前上下文。
     *
     * 为什么不在这里自己创建上下文：WGL / GLX / NSOpenGL 三套平台代码
     * 与「宿主已经有上下文」的场景是互斥的两种集成方式，混在一起会得到
     * 两条都没人完整测过的路径。宿主自建上下文是本项目实际使用的方式，
     * 其余 Kind 一律在 createSurface 明确报 ErrorUnsupported，不做半实现。
     */
    class GlSurface final : public ISurface
    {
    public:
        GlSurface(GlDevice* device, const SurfaceDesc& desc, const RhiLogger& logger);

        RhiResult acquireNextImage() override;
        RhiResult present() override;
        RhiResult resize(Extent2D extent) override;

        Extent2D extent() const override { return m_extent; }
        Format colorFormat() const override { return m_colorFormat; }
        Format depthFormat() const override { return m_depthFormat; }

        /// GL 默认帧缓冲没有对应的纹理对象，返回无效句柄。
        /// RenderPassBeginDesc 的附件留空即表示「画到表面」。
        TextureHandle currentColorTexture() const override { return TextureHandle{}; }
        TextureHandle depthTexture() const override { return TextureHandle{}; }

        /// acquireNextImage 时捕获的当前帧缓冲。QOpenGLWidget 渲染到自己的
        /// FBO 而非 0 号帧缓冲，因此不能写死 0。
        GLuint defaultFramebuffer() const { return m_defaultFramebuffer; }
        uint64_t presentCount() const { return m_presentCount; }
        /// 请求的呈现模式。ForeignGlContext 下实际 vsync 由宿主设置
        /// （Qt: QSurfaceFormat::setSwapInterval），此值仅供诊断。
        PresentMode presentMode() const { return m_presentMode; }

    private:
        GlDevice* m_device = nullptr;
        NativeWindow m_window{};
        Extent2D m_extent{};
        Format m_colorFormat = Format::RGBA8Unorm;
        Format m_depthFormat = Format::Unknown;
        PresentMode m_presentMode = PresentMode::Fifo;
        GLuint m_defaultFramebuffer = 0;
        bool m_acquired = false;
        uint64_t m_presentCount = 0;
        RhiLogger m_log;
    };

    /**
     * @brief GL 命令记录器
     *
     * GL 没有真正的命令缓冲，所有调用即时下发；本类的职责是把
     * 「记录模型」的语义映射到即时 API 上，并在此集中处理状态冗余消除。
     */
    class GlCommandList final : public ICommandList
    {
    public:
        explicit GlCommandList(GlDevice* device);

        /// 每帧由 GlDevice::beginFrame 调用，重置统计与缓存状态
        void beginFrame(GlSurface* surface);
        bool inRenderPass() const { return m_inRenderPass; }

        RhiResult beginRenderPass(const RenderPassBeginDesc& desc) override;
        void endRenderPass() override;
        void bindPipeline(PipelineHandle pipeline) override;
        void setViewport(const Viewport& viewport) override;
        void setScissor(const Rect2D& rect) override;
        void bindVertexBuffer(uint32_t slot, BufferHandle buffer, uint64_t offsetBytes) override;
        void bindIndexBuffer(BufferHandle buffer, uint64_t offsetBytes, IndexType type) override;
        void bindBindGroup(uint32_t set, BindGroupHandle group) override;
        void pushConstants(uint32_t offsetBytes, uint32_t sizeBytes, const void* data) override;
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex,
                  uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex,
                         int32_t vertexOffset, uint32_t firstInstance) override;
        void drawIndirect(BufferHandle argsBuffer, uint64_t offsetBytes, uint32_t drawCount,
                          uint32_t strideBytes) override;
        void drawIndexedIndirect(BufferHandle argsBuffer, uint64_t offsetBytes, uint32_t drawCount,
                                 uint32_t strideBytes) override;
        void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
        void barrier(BarrierScope before, BarrierScope after) override;
        void copyBuffer(BufferHandle src, uint64_t srcOffset, BufferHandle dst, uint64_t dstOffset,
                        uint64_t sizeBytes) override;
        void copyTextureToBuffer(TextureHandle src, BufferHandle dst, uint64_t dstOffset,
                                 const Rect2D& region) override;
        void pushDebugGroup(const char* name) override;
        void popDebugGroup() override;
        FrameStats stats() const override { return m_stats; }

    private:
        /// 绘制前的公共准备：校验状态、刷新顶点绑定与 pushConstant
        bool prepareDraw(const char* what);
        void applyPipelineState(const GlPipelineRecord& pipeline);
        void flushVertexBindings();
        void flushPushConstants();
        /// 解析本次 RenderPass 的目标帧缓冲；附件为空时返回表面的默认帧缓冲
        GLuint resolveFramebuffer(const RenderPassBeginDesc& desc);

        struct VertexBinding
        {
            BufferHandle buffer{};
            uint64_t offset = 0;
        };

        GlDevice* m_device = nullptr;
        GlSurface* m_surface = nullptr;
        FrameStats m_stats{};
        bool m_inRenderPass = false;
        /// 当前 RenderPass 目标尺寸：视口/裁剪的 y 翻转需要它
        Extent2D m_passExtent{};

        PipelineHandle m_pipelineHandle{};
        const GlPipelineRecord* m_pipeline = nullptr;

        VertexBinding m_vertexBindings[kMaxVertexBufferSlots]{};
        bool m_vertexBindingsDirty = false;

        // 索引缓冲是否已绑定，以 m_indexBuffer.valid() 判定：
        // IndexType 只有 Uint16/Uint32 两个取值，没有「未绑定」态，
        // 这是刻意的——后端不该用枚举默认值兼作「无」的哨兵。
        BufferHandle m_indexBuffer{};
        uint64_t m_indexOffset = 0;
        IndexType m_indexType = IndexType::Uint32;

        uint8_t m_pushConstants[kMaxPushConstantBytes]{};
        uint32_t m_pushConstantHighWater = 0;
        bool m_pushConstantsDirty = false;
    };

    /**
     * @brief GL 设备
     *
     * 一个设备 = 一个 GL 上下文 + 该上下文内的全部资源。多窗口共享资源时
     * 宿主应共享同一个上下文（Qt 侧用同一个 QOpenGLContext 或设置共享），
     * 并为每个窗口创建一个 GlSurface。
     */
    class GlDevice final : public IGpuDevice
    {
    public:
        GlDevice(const DeviceDesc& desc, const GLFuncs& functions);
        ~GlDevice() override;

        const Capabilities& capabilities() const override { return m_caps; }

        ISurface* createSurface(const SurfaceDesc& desc) override;
        void destroySurface(ISurface* surface) override;

        ShaderHandle createShader(const ShaderDesc& desc) override;
        void destroyShader(ShaderHandle shader) override;

        PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) override;
        PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) override;
        void destroyPipeline(PipelineHandle pipeline) override;

        BufferHandle createBuffer(const BufferDesc& desc) override;
        void destroyBuffer(BufferHandle buffer) override;
        RhiResult writeBuffer(BufferHandle buffer, uint64_t offset, const void* data,
                              uint64_t sizeBytes) override;
        MappedRange mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes) override;
        void unmapBuffer(BufferHandle buffer) override;
        void flushMappedRange(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes) override;

        TextureHandle createTexture(const TextureDesc& desc) override;
        void destroyTexture(TextureHandle texture) override;
        RhiResult writeTexture(TextureHandle texture, uint32_t mipLevel, const Rect2D& region,
                               const void* data, uint64_t sizeBytes) override;

        SamplerHandle createSampler(const SamplerDesc& desc) override;
        void destroySampler(SamplerHandle sampler) override;

        BindGroupHandle createBindGroup(const BindGroupDesc& desc) override;
        void destroyBindGroup(BindGroupHandle group) override;

        ICommandList* beginFrame(ISurface* surface) override;
        RhiResult submitFrame() override;
        void waitIdle() override;

        RhiResult readTexture(TextureHandle texture, const Rect2D& region, void* outPixels,
                              uint64_t bufferSize, uint32_t* outRowPitch) override;
        uint64_t gpuMemoryUsageBytes() const override;

        // ---------- 供 GlSurface / GlCommandList 使用的内部接口 ----------

        const GLFuncs& gl() const { return m_gl; }
        const RhiLogger& log() const { return m_log; }

        GlBufferRecord* bufferRecord(BufferHandle h) { return m_buffers.get(h); }
        GlTextureRecord* textureRecord(TextureHandle h) { return m_textures.get(h); }
        GlSamplerRecord* samplerRecord(SamplerHandle h) { return m_samplers.get(h); }
        GlPipelineRecord* pipelineRecord(PipelineHandle h) { return m_pipelines.get(h); }
        GlBindGroupRecord* bindGroupRecord(BindGroupHandle h) { return m_bindGroups.get(h); }

        /// pushConstants 在 GL 上落地为一个 UBO，绑定到 kPushConstantBinding
        GLuint pushConstantUbo() const { return m_pushConstantUbo; }

        /// 为一组附件取得（并缓存）FBO；附件组合首次出现时创建
        GLuint framebufferFor(const RenderPassBeginDesc& desc);

        /// pushConstant 块固定占用 0 号 UBO binding point，
        /// 用户声明的 UniformBuffer 从 1 开始顺序分配
        static constexpr uint32_t kPushConstantBinding = 0;
        static constexpr const char* kPushConstantBlockName = "PushConstants";

    private:
        void queryCapabilities();
        /// 编译单个阶段；失败时把 InfoLog 完整记进日志（旧实现只记前 512 字节）
        GLuint compileStage(GLenum stage, const GlShaderRecord& record, const char* debugName);
        void deleteAllResources();

        GLFuncs m_gl{};
        RhiLogger m_log;
        Capabilities m_caps{};
        GlCommandList m_commands;

        std::vector<GlSurface*> m_surfaces;
        ResourcePool<BufferHandle, GlBufferRecord> m_buffers;
        ResourcePool<TextureHandle, GlTextureRecord> m_textures;
        ResourcePool<SamplerHandle, GlSamplerRecord> m_samplers;
        ResourcePool<ShaderHandle, GlShaderRecord> m_shaders;
        ResourcePool<PipelineHandle, GlPipelineRecord> m_pipelines;
        ResourcePool<BindGroupHandle, GlBindGroupRecord> m_bindGroups;

        /// 附件组合 → FBO 缓存。key 由附件的 GL 名字拼成。
        struct FramebufferCacheEntry
        {
            GLuint colorNames[kMaxColorAttachments]{};
            uint32_t colorCount = 0;
            GLuint depthName = 0;
            GLuint fbo = 0;
        };
        std::vector<FramebufferCacheEntry> m_framebufferCache;

        GLuint m_pushConstantUbo = 0;
        GLuint m_readbackFbo = 0;
        bool m_inFrame = false;
        GlSurface* m_frameSurface = nullptr;
        uint64_t m_frameIndex = 0;
    };

}  // namespace Render::RHI::gl
