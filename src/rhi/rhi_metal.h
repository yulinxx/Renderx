/**
 * @file rhi_metal.h
 * @brief Metal render device implementation declaration
 *
 * Implements the IDevice interface using Apple's Metal API.
 * Provides cross-platform GPU rendering on macOS/iOS with native performance.
 *
 * This backend requires macOS 10.11+ or iOS 8+ and uses Objective-C++ (.mm)
 * for Metal API interop. The header itself is pure C++ for ABI compatibility.
 */
#pragma once
#include "rhi_device.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace render::rhi
{

    /**
     * @brief Metal render device implementation
     *
     * Wraps Metal API (MTLDevice, MTLCommandQueue, etc.) to implement
     * the IDevice interface. Uses Objective-C++ internally for Metal interop.
     */
    class MetalDevice : public IDevice
    {
    public:
        MetalDevice();
        ~MetalDevice() override;

        // IDevice interface implementation
        bool initialize(void* nativeWindow, uint32_t width, uint32_t height) override;
        void shutdown() override;

        BufferHandle createBuffer(const BufferDesc&) override;
        void destroyBuffer(BufferHandle) override;
        TextureHandle createTexture(const TextureDesc&) override;
        void destroyTexture(TextureHandle) override;
        PipelineHandle createPipeline(const PipelineDesc&) override;
        void destroyPipeline(PipelineHandle) override;

        void uploadBuffer(BufferHandle, uint64_t offset, uint64_t size, const void* data) override;
        void uploadTexture(TextureHandle, uint32_t mip, const void* data, uint32_t rowPitch) override;
        void* mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t mapFlags) override;
        void unmapBuffer(BufferHandle) override;
        void flushMappedRange(BufferHandle, uint64_t offset, uint64_t size) override;

        void beginFrame() override;
        void endFrame() override;
        void present() override;

        void bindPipeline(PipelineHandle) override;
        void bindVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) override;
        void bindIndexBuffer(BufferHandle, uint64_t offset) override;
        void bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindShaderStorageBuffer(
            uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindTexture(uint32_t set, uint32_t binding, TextureHandle) override;
        void setViewport(const Viewport&) override;
        void setScissor(const Scissor&) override;
        void setLineWidth(float width) override;

        void setUniformMatrix3(const char* name, const float* data) override;
        void setUniformMatrix4(const char* name, const float* data) override;
        void setUniformFloat(const char* name, float value) override;
        void setUniformInt(const char* name, int32_t value) override;
        void setUniformVec2(const char* name, const float* data) override;
        void setUniformVec3(const char* name, const float* data) override;
        void setUniformVec4(const char* name, const float* data) override;

        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount,
            uint32_t instanceCount,
            uint32_t firstIndex,
            int32_t vertexOffset,
            uint32_t firstInstance) override;
        void drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawIndexedIndirect(
            BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;

        void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
        void memoryBarrier(uint32_t barrierFlags) override;

        void setClearColor(float r, float g, float b, float a) override;
        void clear(uint32_t flags) override;
        void enableDepthTest(bool enable) override;
        void enableBlend(bool enable) override;

        // 离屏渲染目标（Metal 后端暂未实现，返回无效 / 空操作）
        RenderTargetHandle createRenderTarget(const RenderTargetDesc&) override;
        void destroyRenderTarget(RenderTargetHandle) override;
        void bindRenderTarget(RenderTargetHandle) override;
        void bindDefaultTarget() override;
        void readRenderTarget(RenderTargetHandle, void*, uint32_t) override;

        void resize(uint32_t width, uint32_t height) override;
        uint64_t getGPUMemoryUsage() const override;
        void* getNativeContext() override;

    private:
        // Metal object handles (stored as opaque pointers, managed in .mm file)
        void* m_device = nullptr;                // MTLDevice*
        void* m_commandQueue = nullptr;          // MTLCommandQueue*
        void* m_renderPassDescriptor = nullptr;  // MTLRenderPassDescriptor*
        void* m_currentDrawable = nullptr;       // id<CAMetalDrawable>
        void* m_currentRenderEncoder = nullptr;  // MTLRenderCommandEncoder*
        void* m_depthStencilState = nullptr;     // MTLDepthStencilState*
        void* m_defaultLibrary = nullptr;        // MTLLibrary*

        // Synchronization
        void* m_framebufferOnly = nullptr;
        uint32_t m_frameIndex = 0;

        // State tracking
        bool m_initialized = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        float m_clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
        float m_lineWidth = 1.0f;
        bool m_depthTestEnabled = false;
        bool m_blendEnabled = false;

        // Handle counters (same scheme as Vulkan backend)
        uint64_t m_nextBufferId = 1;
        uint64_t m_nextTextureId = 1;
        uint64_t m_nextPipelineId = 1;
        uint32_t m_drawCallCount = 0;

        // Internal resource tracking (forward declared types in impl)
        struct BufferResource;
        struct TextureResource;
        struct PipelineResource;

        // Resource maps
        std::unordered_map<uint64_t, std::unique_ptr<BufferResource>> m_buffers;
        std::unordered_map<uint64_t, std::unique_ptr<TextureResource>> m_textures;
        std::unordered_map<uint64_t, std::unique_ptr<PipelineResource>> m_pipelines;

        // Uniform/stage value cache
        std::vector<float> m_uniformCache;

    private:
        // Internal helpers (implemented in .mm)
        void createRenderPass();
        void destroyRenderPass();
        void createDepthBuffer();
        void destroyDepthBuffer();
        bool updateDrawable();
        void setupRenderEncoder();
        void endRenderEncoder();

        // Handle encoding (same scheme as Vulkan)
        static constexpr uint64_t kTypeMask = 0xF000000000000000ULL;
        static constexpr uint64_t kIndexMask = 0x0FFFFFFFFFFFFFFFULL;
        static constexpr uint64_t kTypeShift = 60;

        static uint64_t makeHandle(uint64_t typeTag, uint64_t index)
        {
            return typeTag | (index & kIndexMask);
        }

        static uint64_t handleIndex(uint64_t h)
        {
            return h & kIndexMask;
        }
    };

    /**
     * @brief 创建 Metal 渲染设备实例
     *
     * @return Metal 设备实例指针，失败返回 nullptr
     */
    IDevice* createMetalDevice();

}  // namespace render::rhi
