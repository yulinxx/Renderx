/**
 * @file rhi_metal.h
 * @brief Metal 渲染设备实现声明
 *
 * 使用 Apple 的 Metal API 实现 IDevice 接口。
 * 在 macOS/iOS 上提供跨平台 GPU 渲染，具有原生性能。
 *
 * 此后端需要 macOS 10.11+ 或 iOS 8+，使用 Objective-C++ (.mm)
 * 进行 Metal API 互操作。头文件本身是纯 C++，以保证 ABI 兼容性。
 */
#pragma once
#include "rhiDevice.h"

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace Render::RHI
{

    /**
     * @brief Metal 渲染设备实现
     *
     * 封装 Metal API（MTLDevice、MTLCommandQueue 等）以实现
     * IDevice 接口。内部使用 Objective-C++ 进行 Metal 互操作。
     */
    class MetalDevice : public IDevice
    {
    public:
        MetalDevice();
        ~MetalDevice() override;

        // IDevice 接口实现
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
        int readPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            void* outPixels, uint32_t* outRowPitch) override;
        uint64_t getGPUMemoryUsage() const override;
        void* getNativeContext() override;

    private:
        // Metal 对象句柄（存储为不透明指针，在 .mm 文件中管理）
        void* m_device = nullptr;                // MTLDevice*
        void* m_commandQueue = nullptr;          // MTLCommandQueue*
        void* m_renderPassDescriptor = nullptr;  // MTLRenderPassDescriptor*
        void* m_currentDrawable = nullptr;       // id<CAMetalDrawable>
        void* m_currentRenderEncoder = nullptr;  // MTLRenderCommandEncoder*
        void* m_depthStencilState = nullptr;     // MTLDepthStencilState*
        void* m_defaultLibrary = nullptr;        // MTLLibrary*

        // 同步
        void* m_framebufferOnly = nullptr;
        uint32_t m_frameIndex = 0;

        // 状态跟踪
        bool m_initialized = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        float m_clearColor[4] = { 0.f, 0.f, 0.f, 1.f };
        float m_lineWidth = 1.0f;
        bool m_depthTestEnabled = false;
        bool m_blendEnabled = false;

        // 句柄计数器（与 Vulkan 后端相同的方案）
        uint64_t m_nextBufferId = 1;
        uint64_t m_nextTextureId = 1;
        uint64_t m_nextPipelineId = 1;
        uint32_t m_drawCallCount = 0;

        // 内部资源跟踪（前向声明类型在实现中）
        struct BufferResource;
        struct TextureResource;
        struct PipelineResource;

        // 资源映射
        std::unordered_map<uint64_t, std::unique_ptr<BufferResource>> m_buffers;
        std::unordered_map<uint64_t, std::unique_ptr<TextureResource>> m_textures;
        std::unordered_map<uint64_t, std::unique_ptr<PipelineResource>> m_pipelines;

        // Uniform/阶段值缓存
        std::vector<float> m_uniformCache;

    private:
        // 内部辅助函数（在 .mm 中实现）
        void createRenderPass();
        void destroyRenderPass();
        void createDepthBuffer();
        void destroyDepthBuffer();
        bool updateDrawable();
        void setupRenderEncoder();
        void endRenderEncoder();

        // 句柄编码（与 Vulkan 相同的方案）
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

}  // namespace Render::RHI
