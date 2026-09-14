/**
 * @file metalDevice.h
 * @brief Metal 后端：设备 / 表面 / 命令记录器声明（仅 Apple 平台编译）
 *
 * 与 glDevice.h 的对应关系是逐项对齐的，差异只有三处，且都源于后端本质：
 *
 * 1. **真录制而非伪录制**。GL 后端把「记录模型」映射到即时调用，submitFrame
 *    是空操作；Metal 必须真的把命令录进 MTLCommandBuffer，再由 present 提交。
 *    因此 present 承担了 GL 侧不存在的职责：presentDrawable + commit。
 * 2. **表面不是「默认帧缓冲」而是 CAMetalLayer 的 drawable**。GL 的
 *    currentColorTexture() 只能返回无效句柄（0 号 FBO 没有纹理对象），
 *    而 Metal 侧 drawable 本身就是纹理，可以给出有效句柄供读回使用。
 * 3. **资源对象由 ARC 持有**。destroy* 只把记录从池里摘除。
 *
 * 线程契约与 rhiSurface.h 一致：一个 MetalDevice 及其派生对象只能在
 * 创建它的线程上使用。RHI 自身不加锁。
 *
 * 本头文件包含 ObjC 类型，因此**只能被 .mm 包含**。C++ 侧（rhiFactory.cpp）
 * 只通过 rhiBackendFactory.h 的 C++ 声明访问，不包含本文件。
 */
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "rhi/metal/metalCommon.h"
#include "rhi/rhiCommandList.h"
#include "rhi/rhiGpuDevice.h"
#include "rhi/rhiLog.h"
#include "rhi/rhiResourcePool.h"
#include "rhi/rhiSurface.h"

#include <vector>

namespace Render::RHI::metal
{

    class MetalDevice;

    /**
     * @brief Metal 表面
     *
     * 只支持 NativeWindow::Kind::CocoaNsView：在宿主给定的 NSView 上挂一个
     * CAMetalLayer 并独占它。与 GL 侧只支持 ForeignGlContext 是同一种取舍——
     * 每多支持一种窗口形态就多一条没人完整测过的路径。
     *
     * 交换链以「3 帧 in flight」表达：in-flight 信号量限制 CPU 至多领先 GPU
     * 三帧，避免 CPU 无界地往队列里塞命令（旧实现没有这层，表现为高帧率下
     * 内存持续增长）。
     */
    class MetalSurface final : public ISurface
    {
    public:
        MetalSurface(MetalDevice* device, const SurfaceDesc& desc, const RhiLogger& logger);
        ~MetalSurface() override;

        RhiResult acquireNextImage() override;
        RhiResult present() override;
        RhiResult resize(Extent2D extent) override;

        Extent2D extent() const override { return m_extent; }
        Format colorFormat() const override { return m_colorFormat; }
        Format depthFormat() const override { return m_depthFormat; }

        TextureHandle currentColorTexture() const override { return m_colorTexture; }
        TextureHandle depthTexture() const override { return m_depthTexture; }

        /// 本帧取得的 drawable 纹理；未 acquire 时为 nil
        id<MTLTexture> currentDrawableTexture() const
        {
            return m_drawable != nil ? m_drawable.texture : nil;
        }
        CAMetalLayer* layer() const { return m_layer; }
        uint64_t presentCount() const { return m_presentCount; }

    private:
        void ensureDepthTexture();
        void releaseDrawable();

        MetalDevice* m_device = nullptr;
        NativeWindow m_window{};
        Extent2D m_extent{};
        Format m_colorFormat = Format::BGRA8Unorm;
        Format m_depthFormat = Format::Unknown;

        CAMetalLayer* m_layer = nil;
        dispatch_semaphore_t m_inFlight = nil;
        /// 本帧的 drawable。必须持有到 present：nextDrawable 返回的对象若被
        /// 提前释放，presentDrawable 会作用在一个已回收的 drawable 上。
        id<CAMetalDrawable> m_drawable = nil;
        TextureHandle m_colorTexture{};
        TextureHandle m_depthTexture{};
        id<MTLTexture> m_depthNative = nil;
        bool m_acquired = false;
        uint64_t m_presentCount = 0;
        RhiLogger m_log;
    };

    /**
     * @brief 管线记录
     *
     * Metal 的管线状态分两半：不可变部分（着色器、混合、深度、顶点描述）固化在
     * MTLRenderPipelineState 里；可变部分（视口、裁剪、顶点缓冲绑定、深度偏移）
     * 由命令编码器下发。因此这里只存前者，外加绘制时要用到的元数据。
     */
    struct MetalPipelineRecord
    {
        id<MTLRenderPipelineState> state = nil;
        /// 深度/模板状态。Metal 把深度测试放在 MTLDepthStencilState 里由编码器下发，
        /// 而不是固化进管线状态对象，因此必须随记录一起保存。
        id<MTLDepthStencilState> depthStencilState = nil;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        uint32_t attributeCount = 0;
        VertexAttribute attributes[kMaxVertexAttributes]{};
        uint32_t bufferLayoutCount = 0;
        VertexBufferLayout bufferLayouts[kMaxVertexBufferSlots]{};
        RasterState raster{};
        DepthStencilState depthStencil{};
        uint32_t pushConstantBytes = 0;
    };

    /**
     * @brief Metal 命令记录器
     *
     * 与 GlCommandList 的差异：状态不是「下发给全局状态机」，而是写进
     * MTLRenderCommandEncoder。因此顶点绑定、pushConstant、绑定组都必须
     * 在 draw 之前真正下发到编码器，而不是像 GL 那样可以延迟到绘制前统一 flush。
     */
    class MetalCommandList final : public ICommandList
    {
    public:
        explicit MetalCommandList(MetalDevice* device);

        /// 每帧由 MetalDevice::beginFrame 调用，绑定本帧的命令缓冲
        void beginFrame(MetalSurface* surface, id<MTLCommandBuffer> commandBuffer);

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

        id<MTLCommandBuffer> commandBuffer() const { return m_commandBuffer; }
        bool inRenderPass() const { return m_renderEncoder != nil; }
        Extent2D passExtent() const { return m_passExtent; }

    private:
        /// 顶点缓冲绑定（slot -> buffer/offset）。Metal 必须在 draw 前真正
        /// setVertexBuffer，无法像 GL 那样让状态机自动生效。
        struct VertexBinding
        {
            BufferHandle buffer{};
            uint64_t offset = 0;
        };

        void flushVertexBindings();
        void flushPushConstants();
        bool prepareDraw(const char* what);

        MetalDevice* m_device = nullptr;
        MetalSurface* m_surface = nullptr;
        id<MTLCommandBuffer> m_commandBuffer = nil;
        id<MTLRenderCommandEncoder> m_renderEncoder = nil;
        FrameStats m_stats{};
        Extent2D m_passExtent{};

        PipelineHandle m_pipelineHandle{};
        const MetalPipelineRecord* m_pipeline = nullptr;

        VertexBinding m_vertexBindings[kMaxVertexBufferSlots]{};
        bool m_vertexBindingsDirty = false;

        BufferHandle m_indexBuffer{};
        uint64_t m_indexOffset = 0;
        IndexType m_indexType = IndexType::Uint16;

        /// pushConstant 累积缓冲：RHI 允许分片推送（offset + size），而 Metal 的
        /// setBytes 是整块下发，因此先攒满 128 字节，绘制前一次性发。
        uint8_t m_pushConstants[kMaxPushConstantBytes]{};
        uint32_t m_pushConstantHighWater = 0;
        bool m_pushConstantsDirty = false;
    };

    /**
     * @brief Metal 设备
     *
     * 一个设备 = 一个 MTLDevice + 一个 MTLCommandQueue + 该设备内的全部资源。
     * 多窗口共享资源的方式是：同一个 MetalDevice 上创建多个 MetalSurface。
     */
    class MetalDevice final : public IGpuDevice
    {
    public:
        explicit MetalDevice(const DeviceDesc& desc);
        ~MetalDevice() override;

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

        // ---------- 供 MetalSurface / MetalCommandList 使用的内部接口 ----------

        id<MTLDevice> nativeDevice() const { return m_device; }
        id<MTLCommandQueue> nativeQueue() const { return m_queue; }
        const RhiLogger& log() const { return m_log; }
        MetalCommandList& commands() { return m_commands; }

        MetalBufferRecord* bufferRecord(BufferHandle h) { return m_buffers.get(h); }
        MetalTextureRecord* textureRecord(TextureHandle h) { return m_textures.get(h); }
        MetalSamplerRecord* samplerRecord(SamplerHandle h) { return m_samplers.get(h); }
        MetalShaderRecord* shaderRecord(ShaderHandle h) { return m_shaders.get(h); }
        MetalBindGroupRecord* bindGroupRecord(BindGroupHandle h) { return m_bindGroups.get(h); }
        MetalPipelineRecord* pipelineRecord(PipelineHandle h) { return m_pipelines.get(h); }

        /**
         * @brief 更新（必要时创建）由表面托管的纹理记录，返回该句柄
         *
         * drawable 的纹理每帧都被 nextDrawable 替换，但句柄在表面生命周期内
         * 保持稳定：rxSessionReadPixels 用的是「当前后备缓冲」语义，句柄必须
         * 跨帧可用。句柄由**调用方（MetalSurface）持有**——放在设备上会在
         * 多窗口时互相覆盖，这是多窗口下最难查的一类错误。
         */
        TextureHandle updateBoundTexture(TextureHandle handle, id<MTLTexture> texture,
                                         const TextureDesc& desc);

        bool inFrame() const { return m_inFrame; }
        MetalSurface* frameSurface() const { return m_frameSurface; }
        uint64_t frameIndex() const { return m_frameIndex; }

    private:
        void queryCapabilities();

        id<MTLDevice> m_device = nil;
        id<MTLCommandQueue> m_queue = nil;
        RhiLogger m_log;
        Capabilities m_caps{};
        MetalCommandList m_commands;

        std::vector<MetalSurface*> m_surfaces;
        ResourcePool<BufferHandle, MetalBufferRecord> m_buffers;
        ResourcePool<TextureHandle, MetalTextureRecord> m_textures;
        ResourcePool<SamplerHandle, MetalSamplerRecord> m_samplers;
        ResourcePool<ShaderHandle, MetalShaderRecord> m_shaders;
        ResourcePool<BindGroupHandle, MetalBindGroupRecord> m_bindGroups;
        ResourcePool<PipelineHandle, MetalPipelineRecord> m_pipelines;

        bool m_inFrame = false;
        MetalSurface* m_frameSurface = nullptr;
        uint64_t m_frameIndex = 0;
    };

}  // namespace Render::RHI::metal
