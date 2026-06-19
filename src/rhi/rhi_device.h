#pragma once
#include "rhi_types.h"
#include <cstddef>

namespace render::rhi {

class IDevice {
public:
    virtual ~IDevice() = default;

    virtual bool initialize(void* nativeWindow, uint32_t width, uint32_t height) = 0;
    virtual void shutdown() = 0;

    virtual BufferHandle   createBuffer(const BufferDesc&) = 0;
    virtual void           destroyBuffer(BufferHandle) = 0;
    virtual TextureHandle  createTexture(const TextureDesc&) = 0;
    virtual void           destroyTexture(TextureHandle) = 0;
    virtual PipelineHandle createPipeline(const PipelineDesc&) = 0;
    virtual void           destroyPipeline(PipelineHandle) = 0;

    virtual void uploadBuffer(BufferHandle, uint64_t offset, uint64_t size, const void* data) = 0;
    virtual void uploadTexture(TextureHandle, uint32_t mip, const void* data, uint32_t rowPitch) = 0;
    virtual void* mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t mapFlags) = 0;
    virtual void unmapBuffer(BufferHandle) = 0;
    virtual void flushMappedRange(BufferHandle, uint64_t offset, uint64_t size) = 0;

    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
    virtual void present() = 0;

    virtual void bindPipeline(PipelineHandle) = 0;
    virtual void bindVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) = 0;
    virtual void bindIndexBuffer(BufferHandle, uint64_t offset) = 0;
    virtual void bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) = 0;
    virtual void bindTexture(uint32_t set, uint32_t binding, TextureHandle) = 0;
    virtual void setViewport(const Viewport&) = 0;
    virtual void setScissor(const Scissor&) = 0;
    virtual void setLineWidth(float width) = 0;

    virtual void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) = 0;
    virtual void drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;
    virtual void drawIndexedIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) = 0;

    virtual void setClearColor(float r, float g, float b, float a) = 0;
    virtual void clear(uint32_t flags) = 0;
    virtual void enableDepthTest(bool enable) = 0;
    virtual void enableBlend(bool enable) = 0;

    virtual void resize(uint32_t width, uint32_t height) = 0;

    virtual uint64_t getGPUMemoryUsage() const = 0;
    virtual void*    getNativeContext() = 0;
};

IDevice* createGLDevice();

}
