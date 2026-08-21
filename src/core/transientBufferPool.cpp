/**
 * @file transientbufferpool.cpp
 * @brief TransientBufferPool implementation
 */
#include "transientBufferPool.h"
#include "platform/glLoader.h"
#include "Log/SyLogger.h"

#include <cstdint>

namespace Render
{
    namespace core
    {
        namespace
        {
            // 向上对齐到指定边界
            inline uint64_t align_up(uint64_t value, uint64_t alignment)
            {
                return (value + alignment - 1) & ~(alignment - 1);
            }
        }  // namespace

        TransientBufferPool::~TransientBufferPool()
        {
            if (m_initialized)
            {
                shutdown();
            }
        }

        bool TransientBufferPool::initialize(RHI::IDevice* device, uint64_t bufferSize, uint32_t frameCount)
        {
            if (!device || bufferSize == 0 || frameCount == 0)
            {
                SY_WARN("[TransientBufferPool] Invalid arguments for initialization");
                return false;
            }

            m_device = device;
            m_bufferSize = bufferSize;
            m_frameCount = frameCount;
            m_currentFrame = 0;
            m_fallbackCount = 0;

            m_frames.resize(frameCount);

            // 为每个帧槽创建持久映射缓冲区
            for (uint32_t i = 0; i < frameCount; ++i)
            {
                RHI::BufferDesc desc;
                desc.size = bufferSize;
                desc.usage = RHI::BufferUsage::Vertex;
                desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
                desc.debugName = "TransientBufferPool_Frame";

                m_frames[i].handle = device->createBuffer(desc);
                if (m_frames[i].handle == RHI::NullHandle)
                {
                    SY_WARNF("[TransientBufferPool] Failed to create buffer for frame %u", i);
                    shutdown();
                    return false;
                }

                // 对整个缓冲区做持久映射，生命周期持续到 shutdown
                uint32_t mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
                m_frames[i].mappedPtr = device->mapBuffer(m_frames[i].handle, 0, bufferSize, mapFlags);
                if (!m_frames[i].mappedPtr)
                {
                    SY_WARNF("[TransientBufferPool] Failed to persistently map buffer for frame %u", i);
                    shutdown();
                    return false;
                }

                m_frames[i].used = 0;
            }

            m_initialized = true;
            SY_DEBUGF("[TransientBufferPool] Initialized with %u frames, each %llu bytes",
                frameCount,
                static_cast<unsigned long long>(bufferSize));
            return true;
        }

        void TransientBufferPool::shutdown()
        {
            if (!m_device)
            {
                return;
            }

            // 释放帧缓冲区及其 fallback
            for (auto& frame : m_frames)
            {
                if (frame.handle != RHI::NullHandle)
                {
                    if (frame.mappedPtr)
                    {
                        m_device->unmapBuffer(frame.handle);
                        frame.mappedPtr = nullptr;
                    }
                    m_device->destroyBuffer(frame.handle);
                    frame.handle = RHI::NullHandle;
                }

                for (auto& fb : frame.fallbacks)
                {
                    if (fb.handle != RHI::NullHandle)
                    {
                        if (fb.mappedPtr)
                        {
                            m_device->unmapBuffer(fb.handle);
                        }
                        m_device->destroyBuffer(fb.handle);
                    }
                }
                frame.fallbacks.clear();
                frame.used = 0;

                // 释放 GPU 同步栅栏
                if (frame.fence)
                {
                    auto* g = gl();
                    if (g->DeleteSync)
                    {
                        g->DeleteSync(static_cast<GLsync>(frame.fence));
                    }
                    frame.fence = nullptr;
                }
            }
            m_frames.clear();

            m_initialized = false;
            m_device = nullptr;
            m_bufferSize = 0;
            m_frameCount = 3;
            m_currentFrame = 0;
            m_fallbackCount = 0;
            m_currentFrameFallbackCount = 0;
            m_consecutiveFallbackFrames = 0;
            SY_DEBUG("[TransientBufferPool] Shutdown complete");
        }

        void TransientBufferPool::beginFrame()
        {
            if (!m_initialized)
            {
                return;
            }

            auto* g = gl();
            FrameBuffer& currentFrame = m_frames[m_currentFrame];

            // 为当前帧槽插入 fence，标记 GPU 对该帧槽的读取边界
            if (g->FenceSync)
            {
                if (currentFrame.fence)
                {
                    if (g->DeleteSync)
                    {
                        g->DeleteSync(static_cast<GLsync>(currentFrame.fence));
                    }
                }
                currentFrame.fence = g->FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            }

            // 轮换到下一帧槽
            m_currentFrame = (m_currentFrame + 1) % m_frameCount;
            FrameBuffer& nextFrame = m_frames[m_currentFrame];

            // 等待下一帧槽的 fence，确保 GPU 已完成对其的读取后再重写
            if (nextFrame.fence && g->ClientWaitSync)
            {
                GLenum result = g->ClientWaitSync(static_cast<GLsync>(nextFrame.fence), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
                if (result == GL_TIMEOUT_EXPIRED)
                {
                    // GPU 仍在读取，阻塞等待（最坏情况下的同步点）
                    SY_WARNF("[TransientBufferPool] Frame %u still in use by GPU, blocking wait...", m_currentFrame);
                    result =
                        g->ClientWaitSync(static_cast<GLsync>(nextFrame.fence), GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000);
                    if (result == GL_TIMEOUT_EXPIRED)
                    {
                        SY_ERRORF("[TransientBufferPool] Frame %u GPU wait timeout (1s), potential data corruption",
                            m_currentFrame);
                    }
                }
                if (g->DeleteSync)
                {
                    g->DeleteSync(static_cast<GLsync>(nextFrame.fence));
                }
                nextFrame.fence = nullptr;
            }

            // 重置主缓冲区偏移
            nextFrame.used = 0;

            // 清理该帧槽之前积累的 fallback 缓冲区
            for (auto& fb : nextFrame.fallbacks)
            {
                if (fb.handle != RHI::NullHandle)
                {
                    if (fb.mappedPtr)
                    {
                        m_device->unmapBuffer(fb.handle);
                    }
                    m_device->destroyBuffer(fb.handle);
                }
            }
            nextFrame.fallbacks.clear();

            // 监控 fallback 退化情况
            if (m_currentFrameFallbackCount > 0)
            {
                ++m_consecutiveFallbackFrames;
                if (m_consecutiveFallbackFrames >= 3 && !m_fallbackWarningReported)
                {
                    m_fallbackWarningReported = true;
                    SY_WARNF("[TransientBufferPool] Fallback degenerated for %u consecutive frames (last frame: %u "
                             "fallbacks). Consider increasing buffer size.",
                        m_consecutiveFallbackFrames,
                        m_currentFrameFallbackCount);
                }
            }
            else
            {
                m_consecutiveFallbackFrames = 0;
                m_fallbackWarningReported = false;
            }
            m_currentFrameFallbackCount = 0;
        }

        TransientBufferPool::Allocation TransientBufferPool::allocate(uint64_t size, uint64_t alignment)
        {
            Allocation alloc;
            if (!m_initialized || size == 0)
            {
                return alloc;
            }

            FrameBuffer& frame = m_frames[m_currentFrame];
            uint64_t alignedOffset = align_up(frame.used, alignment);

            if (alignedOffset + size <= m_bufferSize)
            {
                alloc.buffer = frame.handle;
                alloc.offset = alignedOffset;
                alloc.cpuPtr = static_cast<uint8_t*>(frame.mappedPtr) + alignedOffset;
                alloc.size = size;
                frame.used = alignedOffset + size;
                return alloc;
            }

            // 主缓冲区溢出，回退到临时缓冲区
            SY_WARNF("[TransientBufferPool] Frame %u overflow (used=%llu, req=%llu), using fallback",
                m_currentFrame,
                static_cast<unsigned long long>(frame.used),
                static_cast<unsigned long long>(size));
            return allocateFromFallback(size, alignment);
        }

        TransientBufferPool::Allocation TransientBufferPool::allocateFromFallback(uint64_t size, uint64_t alignment)
        {
            Allocation alloc;
            uint64_t alignedSize = align_up(size, alignment);

            // 创建一个恰好容纳本次分配的临时缓冲区
            RHI::BufferDesc desc;
            desc.size = alignedSize;
            desc.usage = RHI::BufferUsage::Vertex;
            desc.memory = RHI::MemoryType::GPU_CPU_Coherent;
            desc.debugName = "TransientBufferPool_Fallback";

            RHI::BufferHandle handle = m_device->createBuffer(desc);
            if (handle == RHI::NullHandle)
            {
                SY_WARN("[TransientBufferPool] Failed to create fallback buffer");
                return alloc;
            }

            uint32_t mapFlags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
            void* mapped = m_device->mapBuffer(handle, 0, alignedSize, mapFlags);
            if (!mapped)
            {
                SY_WARN("[TransientBufferPool] Failed to map fallback buffer");
                m_device->destroyBuffer(handle);
                return alloc;
            }

            FallbackBuffer fb;
            fb.handle = handle;
            fb.mappedPtr = mapped;
            fb.capacity = alignedSize;

            FrameBuffer& frame = m_frames[m_currentFrame];
            frame.fallbacks.push_back(fb);
            ++m_fallbackCount;
            ++m_currentFrameFallbackCount;

            alloc.buffer = handle;
            alloc.offset = 0;
            alloc.cpuPtr = mapped;
            alloc.size = size;

            SY_DEBUGF("[TransientBufferPool] Fallback allocated (size=%llu, total_fallbacks=%llu)",
                static_cast<unsigned long long>(alignedSize),
                static_cast<unsigned long long>(m_fallbackCount));
            return alloc;
        }

        uint64_t TransientBufferPool::usedSize() const
        {
            if (!m_initialized || m_frames.empty())
            {
                return 0;
            }
            return m_frames[m_currentFrame].used;
        }
    }  // namespace core
}  // namespace Render