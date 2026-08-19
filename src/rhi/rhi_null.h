/**
 * @file rhi_null.h
 * @brief Null render device implementation (no GPU operations)
 *
 * NullDevice implements IDevice interface with no-op operations.
 * Used for:
 * - Unit tests (no OpenGL context required)
 * - CI/CD automated testing
 * - Backend abstraction validation
 * - Pre-validation before adding GPU backends
 *
 * NOTE: This backend performs NO actual GPU operations.
 * All resource creation returns valid virtual handles.
 * All draw calls are no-ops.
 */
#pragma once
#include "rhi_device.h"

#include <cstdint>

namespace render::rhi
{

    class NullDevice : public IDevice
    {
    public:
        NullDevice() = default;
        ~NullDevice() override = default;

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

        void resize(uint32_t width, uint32_t height) override;
        int readPixels(uint32_t x, uint32_t y, uint32_t width, uint32_t height,
            void* outPixels, uint32_t* outRowPitch) override;
        uint64_t getGPUMemoryUsage() const override;
        void* getNativeContext() override;

        // 离屏渲染目标（Null 后端为空实现，返回无效句柄 / 不操作）
        RenderTargetHandle createRenderTarget(const RenderTargetDesc&) override;
        void destroyRenderTarget(RenderTargetHandle) override;
        void bindRenderTarget(RenderTargetHandle) override;
        void bindDefaultTarget() override;
        void readRenderTarget(RenderTargetHandle, void*, uint32_t) override;

        // Null backend tracks resource counts for testing validation
        uint64_t getBufferCount() const
        {
            return m_nextBufferId - 1;
        }

        uint64_t getTextureCount() const
        {
            return m_nextTextureId - 1;
        }

        uint64_t getPipelineCount() const
        {
            return m_nextPipelineId - 1;
        }

        uint32_t getDrawCallCount() const
        {
            return m_drawCallCount;
        }

    private:
        BufferHandle allocBufferHandle();
        TextureHandle allocTextureHandle();
        PipelineHandle allocPipelineHandle();

    private:
        bool m_initialized = false;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        float m_clearColor[4] = { 0.f, 0.f, 0.f, 1.f };

        uint64_t m_nextBufferId = 1;
        uint64_t m_nextTextureId = 1;
        uint64_t m_nextPipelineId = 1;

        uint32_t m_drawCallCount = 0;
        bool m_depthTestEnabled = false;
        bool m_blendEnabled = false;
        float m_lineWidth = 1.0f;
    };

}  // namespace render::rhi
