/**
 * @file rhinull.cpp
 * @brief Null render device implementation (no GPU operations)
 *
 * All methods are no-ops or return default values.
 * Used for unit testing without OpenGL context.
 *
 * NOTE: This backend does NOT perform any actual GPU operations.
 * Resource management uses virtual handle allocation for testing.
 */
#include "rhiNull.h"
#include "Log/SyLogger.h"

namespace Render::RHI
{
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

    void* NullDevice::mapBuffer(BufferHandle, uint64_t /*offset*/, uint64_t /*size*/, uint32_t)
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

    void NullDevice::draw(uint32_t, uint32_t, uint32_t, uint32_t)
    {
        m_drawCallCount++;
    }

    void NullDevice::drawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
    {
        m_drawCallCount++;
    }

    void NullDevice::drawIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t)
    {
        m_drawCallCount += drawCount;
    }

    void NullDevice::drawIndexedIndirect(BufferHandle, uint64_t, uint32_t drawCount, uint32_t)
    {
        m_drawCallCount += drawCount;
    }

    // ==================== Compute ====================

    void NullDevice::dispatchCompute(uint32_t, uint32_t, uint32_t) {}

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

    int NullDevice::readPixels(
        uint32_t /*x*/, uint32_t /*y*/, uint32_t width, uint32_t /*height*/, void* /*outPixels*/, uint32_t* outRowPitch)
    {
        if (outRowPitch)
        {
            *outRowPitch = ((width * 4 + 3) / 4) * 4;
        }
        return 1;  // Null backend always succeeds
    }

    uint64_t NullDevice::getGPUMemoryUsage() const
    {
        return 0;
    }

    void* NullDevice::getNativeContext()
    {
        return nullptr;
    }

    bool NullDevice::checkFence(uint64_t /*fenceValue*/) const
    {
        return true;  // Null backend: 无 GPU 异步回读，fence 总是视为已触发
    }

    // 离屏渲染目标：Null 后端无任何 GPU 资源，均返回无效 / 空操作
    RenderTargetHandle NullDevice::createRenderTarget(const RenderTargetDesc&)
    {
        return NullRenderTarget;
    }

    void NullDevice::destroyRenderTarget(RenderTargetHandle) {}

    void NullDevice::bindRenderTarget(RenderTargetHandle) {}

    void NullDevice::bindDefaultTarget() {}

    void NullDevice::readRenderTarget(RenderTargetHandle, void*, uint32_t) {}
}  // namespace Render::RHI

// ==================== Factory Function ====================

namespace Render::RHI
{
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
}  // namespace Render::RHI