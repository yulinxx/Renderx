/**
 * @file transient_buffer_pool.h
 * @brief Transient Buffer Pool using Persistent Mapped Buffers
 *
 * 使用 OpenGL 4.4+ 的 Persistent Mapped Buffer 实现零拷贝 CPU->GPU 数据传输。
 * 采用 Triple Buffering 策略，每帧轮换一块独占缓冲区，避免 CPU 与 GPU 之间的同步等待。
 *
 * 典型使用流程：
 * 1. initialize() 创建 N 个持久映射缓冲区
 * 2. 每帧开始时调用 beginFrame() 切换到下一帧缓冲区
 * 3. 通过 allocate() 在当前帧缓冲区中分配临时内存
 * 4. CPU 直接写入 Allocation.cpuPtr，GPU 自动可见（coherent）
 * 5. shutdown() 释放所有资源
 */
#pragma once

#include "rhi/rhi_device.h"
#include "rhi/rhi_types.h"
#include <cstdint>
#include <vector>

namespace render {
namespace core {

/**
 * @brief 瞬态缓冲区池
 *
 * 为每帧动态生成的顶点/索引/实例数据提供高速分配通道。
 * 内部使用持久映射缓冲区，CPU 写入后无需显式 upload 或 flush。
 */
class TransientBufferPool {
public:
    /**
     * @brief 分配结果描述符
     */
    struct Allocation {
        rhi::BufferHandle buffer = rhi::NullHandle; ///< RHI 缓冲区句柄
        uint64_t offset = 0;                        ///< 在缓冲区内的字节偏移
        void* cpuPtr = nullptr;                     ///< CPU 可直接写入的内存指针
        uint64_t size = 0;                          ///< 分配大小（字节）
    };

    TransientBufferPool() = default;
    ~TransientBufferPool();

    // 禁止拷贝
    TransientBufferPool(const TransientBufferPool&) = delete;
    TransientBufferPool& operator=(const TransientBufferPool&) = delete;

    /**
     * @brief 初始化缓冲区池
     *
     * 为每个帧槽创建持久映射缓冲区。
     *
     * @param device RHI 设备实例
     * @param bufferSize 每个帧缓冲区的容量（字节）
     * @param frameCount 帧槽数量（默认 3，即 Triple Buffering）
     * @return true 初始化成功
     */
    bool initialize(rhi::IDevice* device, uint64_t bufferSize, uint32_t frameCount = 3);

    /**
     * @brief 释放所有资源
     */
    void shutdown();

    /**
     * @brief 每帧开始时调用，轮换到下一个帧槽并清空其使用记录
     */
    void beginFrame();

    /**
     * @brief 从当前帧缓冲区分配一块内存
     *
     * 分配失败时会自动回退到临时缓冲区（fallback），不会阻塞。
     *
     * @param size 请求大小（字节）
     * @param alignment 对齐要求（默认 256，满足 UBO/SSBO 对齐需求）
     * @return Allocation 描述符；buffer 为 NullHandle 表示失败
     */
    Allocation allocate(uint64_t size, uint64_t alignment = 256);

    /**
     * @brief 获取单个帧缓冲区的总容量
     */
    uint64_t bufferSize() const { return m_bufferSize; }

    /**
     * @brief 获取当前帧缓冲区已使用字节数
     */
    uint64_t usedSize() const;

    /**
     * @brief 获取自初始化以来的 fallback 分配总次数
     */
    uint64_t fallbackCount() const { return m_fallbackCount; }

private:
    struct FallbackBuffer {
        rhi::BufferHandle handle = rhi::NullHandle;
        void* mappedPtr = nullptr;
        uint64_t capacity = 0;
    };

    struct FrameBuffer {
        rhi::BufferHandle handle = rhi::NullHandle;
        void* mappedPtr = nullptr;
        uint64_t used = 0;
        std::vector<FallbackBuffer> fallbacks;
        void* fence = nullptr; ///< GPU 同步栅栏（GLsync），确保旧帧槽在重写前 GPU 已完成读取
    };

    rhi::IDevice* m_device = nullptr;
    uint64_t m_bufferSize = 0;
    uint32_t m_frameCount = 3;
    uint32_t m_currentFrame = 0;
    bool m_initialized = false;

    std::vector<FrameBuffer> m_frames;
    uint64_t m_fallbackCount = 0;
    uint32_t m_currentFrameFallbackCount = 0; ///< 当前帧的 fallback 次数（用于监控退化）
    uint32_t m_consecutiveFallbackFrames = 0; ///< 连续触发 fallback 的帧数

    Allocation allocateFromFallback(uint64_t size, uint64_t alignment);
};

} // namespace core
} // namespace render
