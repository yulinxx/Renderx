/**
 * @file rhi_null.cpp
 * @brief Null render device implementation (no GPU operations)
 *
 * All methods are no-ops or return default values.
 * Used for unit testing without OpenGL context.
 *
 * NOTE: This backend does NOT perform any actual GPU operations.
 * Resource management uses virtual handle allocation for testing.
 */
#include "rhi_null.h"
#include "Log/SyLogger.h"

namespace render::rhi {

// ==================== Lifecycle ====================

bool NullDevice::initialize(void* /*nativeWindow*/, uint32_t width, uint32_t height)
{
    SY_DEBUG("[NullDevice] initialize: no GPU context required");
    m_width = width;
    m_height = height;
    m_initialized = true;
    m_drawCallCount = 0;
    return true;
}

void NullDevice::shutdown()
{
    SY_DEBUG("[NullDevice] shutdown");
    m_initialized = false;
    m_width = 0;
    m_height = 0;
    m_nextBufferId = 1;
    m_nextTextureId = 1;
    m_nextPipelineId = 1;
}

// ==================== Resource Management ====================

BufferHandle NullDevice::allocBufferHandle()
{
    return m_nextBufferId++;
}

TextureHandle NullDevice::allocTextureHandle()
{
    return m_nextTextureId++;
}

PipelineHandle NullDevice::allocPipelineHandle()
{
    return m_nextPipelineId++;
}

BufferHandle NullDevice::createBuffer(const BufferDesc&)
{
    // Null backend: no actual allocation, just return a virtual handle
    return allocBufferHandle();
}

void NullDevice::destroyBuffer(BufferHandle)
{
    // Null backend: no actual deallocation
}

TextureHandle NullDevice::createTexture(const TextureDesc&)
{
    return allocTextureHandle();
}

void NullDevice::destroyTexture(TextureHandle)
{
    // Null backend: no actual deallocation
}

PipelineHandle NullDevice::createPipeline(const PipelineDesc&)
{
    return allocPipelineHandle();
}

void NullDevice::destroyPipeline(PipelineHandle)
{
    // Null backend: no actual deallocation
}

// ==================== Data Upload ====================

void NullDevice::uploadBuffer(BufferHandle, uint64_t, uint64_t, const void*)
{
    // Null backend: data is discarded, no GPU copy
}

void NullDevice::uploadTexture(TextureHandle, uint32_t, const void*, uint32_t)
{
    // Null backend: data is discarded
}

void* NullDevice::mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t)
{
    // Null backend: return nullptr to indicate no CPU-accessible memory
    // Callers should check for nullptr and handle gracefully
    return nullptr;
}

void NullDevice::unmapBuffer(BufferHandle)
{
    // Null backend: no-op
}

void NullDevice::flushMappedRange(BufferHandle, uint64_t, uint64_t)
{
    // Null backend: no-op
}

// ==================== Frame Management ====================

void NullDevice::beginFrame()
{
    // Null backend: no-op, frame counter still increments
}

void NullDevice::endFrame()
{
    // Null backend: no-op
}

void NullDevice::present()
{
    // Null backend: no swap chain, no-op
}

// ==================== State Setting ====================

void NullDevice::bindPipeline(PipelineHandle)
{
    // Null backend: no-op
}

void NullDevice::bindVertexBuffer(uint32_t, BufferHandle, uint64_t)
{
    // Null backend: no-op
}

void NullDevice::bindIndexBuffer(BufferHandle, uint64_t)
{
    // Null backend: no-op
}

void NullDevice::bindUniformBuffer(uint32_t, uint32_t, BufferHandle, uint64_t, uint64_t)
{
    // Null backend: no-op
}

void NullDevice::bindShaderStorageBuffer(uint32_t, uint32_t, BufferHandle, uint64_t, uint64_t)
{
    // Null backend: no-op
}

void NullDevice::bindTexture(uint32_t, uint32_t, TextureHandle)
{
    // Null backend: no-op
}

void NullDevice::setViewport(const Viewport&)
{
    // Null backend: no-op
}

void NullDevice::setScissor(const Scissor&)
{
    // Null backend: no-op
}

void NullDevice::setLineWidth(float width)
{
    m_lineWidth = width;
}

// ==================== Uniform Setting ====================

void NullDevice::setUniformMatrix3(const char*, const float*)
{
    // Null backend: no-op
}

void NullDevice::setUniformMatrix4(const char*, const float*)
{
    // Null backend: no-op
}

void NullDevice::setUniformFloat(const char*, float)
{
    // Null backend: no-op
}

void NullDevice::setUniformInt(const char*, int32_t)
{
    // Null backend: no-op
}

void NullDevice::setUniformVec2(const char*, const float*)
{
    // Null backend: no-op
}

void NullDevice::setUniformVec3(const char*, const float*)
{
    // Null backend: no-op
}

void NullDevice::setUniformVec4(const char*, const float*)
{
    // Null backend: no-op
}

// ==================== Drawing ====================

void NullDevice::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t, uint32_t)
{
    m_drawCallCount++;
    SY_DEBUGF("[NullDevice] draw: vertices=%u, instances=%u", vertexCount, instanceCount);
}

void NullDevice::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t, int32_t, uint32_t)
{
    m_drawCallCount++;
    SY_DEBUGF("[NullDevice] drawIndexed: indices=%u, instances=%u", indexCount, instanceCount);
}

void NullDevice::drawIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t)
{
    m_drawCallCount += drawCount;
    SY_DEBUGF("[NullDevice] drawIndirect: drawCount=%u", drawCount);
}

void NullDevice::drawIndexedIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t)
{
    m_drawCallCount += drawCount;
    SY_DEBUGF("[NullDevice] drawIndexedIndirect: drawCount=%u", drawCount);
}

// ==================== Compute ====================

void NullDevice::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
{
    SY_DEBUGF("[NullDevice] dispatchCompute: %ux%ux%u", groupsX, groupsY, groupsZ);
}

void NullDevice::memoryBarrier(uint32_t)
{
    // Null backend: no-op
}

// ==================== Render State ====================

void NullDevice::setClearColor(float r, float g, float b, float a)
{
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void NullDevice::clear(uint32_t)
{
    // Null backend: no-op (no actual framebuffer to clear)
}

void NullDevice::enableDepthTest(bool enable)
{
    m_depthTestEnabled = enable;
}

void NullDevice::enableBlend(bool enable)
{
    m_blendEnabled = enable;
}

// ==================== Query ====================

void NullDevice::resize(uint32_t width, uint32_t height)
{
    m_width = width;
    m_height = height;
}

uint64_t NullDevice::getGPUMemoryUsage() const
{
    return 0;
}

void* NullDevice::getNativeContext()
{
    return nullptr;
}

}   // namespace render::rhi

// ==================== Factory Function ====================

namespace render::rhi {

/**
 * @brief 创建Null渲染设备实例
 *
 * NullDevice 不执行任何实际的 GPU 操作，所有方法都是空实现或返回默认值。
 * 主要用于单元测试、CI/CD 自动化测试和后端抽象层验证。
 */
IDevice* createNullDevice()
{
    return new NullDevice();
}

}   // namespace render::rhi
