/**
 * @file rhiGpuDevice.h
 * @brief GPU 设备接口与后端工厂
 *
 * 与旧版 IDevice 的核心差异：
 *
 * 1. 设备与窗口解耦。旧版 initialize(void* nativeWindow, w, h) 把设备绑死在
 *    一个窗口上，于是「多窗口」只能变成「多设备」，GPU 资源无法共享，每个
 *    窗口各自持有一份 2048x2048 字体图集与全套管线。现在窗口是
 *    ISurface，由同一个 IGpuDevice 创建任意多个。
 *
 * 2. 无即时模式绘制状态。所有绘制状态与命令进入 ICommandList，
 *    设备只负责资源生命周期与提交。
 *
 * 3. 无字符串 uniform。资源绑定走 BindGroup（descriptor set）+ pushConstants。
 *
 * 4. 显式能力查询（Capabilities），上层不再靠「函数指针是否为空」猜特性。
 *
 * 5. 全部可失败操作返回 RhiResult；资源创建返回句柄，失败时句柄无效。
 */
#pragma once

#include "rhiCommandList.h"
#include "rhiCore.h"
#include "rhiSurface.h"

namespace Render::RHI
{

    /// 日志回调。RHI 不直接依赖任何日志库，由宿主注入。
    enum class LogLevel : uint8_t
    {
        Debug = 0,
        Info,
        Warn,
        Error,
    };

    using LogCallback = void (*)(LogLevel level, const char* message, void* userData);

    struct DeviceDesc
    {
        BackendKind backend = BackendKind::OpenGL;
        bool enableValidation = false;  ///< Vulkan validation layer / GL debug output
        LogCallback logCallback = nullptr;
        void* logUserData = nullptr;
        const char* applicationName = "RenderX";
        /**
         * @brief GL 专用：宿主提供的 getProcAddress，签名为 void*(const char*)
         *
         * 传 nullptr 时使用平台默认实现（Windows: wglGetProcAddress +
         * opengl32.dll；Linux: glXGetProcAddress；macOS: dlsym）。
         * Qt 宿主应传 QOpenGLContext::getProcAddress 的包装：Qt 在某些平台
         * （尤其是 ANGLE / EGL 后端）下的符号解析与平台默认路径不一致。
         *
         * 其他后端忽略此字段。
         */
        void* glGetProcAddress = nullptr;
    };

    /**
     * @brief 映射后的缓冲区区间
     *
     * 由 mapBuffer 返回。ptr 为空表示映射失败。
     */
    struct MappedRange
    {
        void* ptr = nullptr;
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    class IGpuDevice
    {
    public:
        virtual ~IGpuDevice() = default;

        // ---------- 能力 ----------

        virtual const Capabilities& capabilities() const = 0;

        // ---------- 表面（窗口） ----------

        /**
         * @brief 为一个窗口创建可呈现表面
         *
         * 同一设备可创建多个表面，它们共享该设备的全部 GPU 资源
         * （管线、纹理、缓冲、字体图集）。这是多窗口的正确形态。
         */
        virtual ISurface* createSurface(const SurfaceDesc& desc) = 0;
        virtual void destroySurface(ISurface* surface) = 0;

        // ---------- 着色器 ----------

        /**
         * @brief 创建着色器模块
         *
         * desc.language 必须等于 capabilities().acceptedShaderLanguage，
         * 否则返回无效句柄。RHI 不做任何跨语言转译。
         */
        virtual ShaderHandle createShader(const ShaderDesc& desc) = 0;
        virtual void destroyShader(ShaderHandle shader) = 0;

        // ---------- 管线 ----------

        virtual PipelineHandle createGraphicsPipeline(const GraphicsPipelineDesc& desc) = 0;
        virtual PipelineHandle createComputePipeline(const ComputePipelineDesc& desc) = 0;
        virtual void destroyPipeline(PipelineHandle pipeline) = 0;

        // ---------- 缓冲区 ----------

        virtual BufferHandle createBuffer(const BufferDesc& desc) = 0;
        virtual void destroyBuffer(BufferHandle buffer) = 0;

        /**
         * @brief 立即上传数据到缓冲区
         *
         * 适用于低频更新。高频流式写入应使用 MemoryAccess::CpuToGpu
         * 缓冲配合 mapBuffer 持久映射。
         */
        virtual RhiResult writeBuffer(BufferHandle buffer, uint64_t offset, const void* data,
                                      uint64_t sizeBytes) = 0;

        /// 仅对 CpuToGpu / GpuToCpu / CpuToGpuCoherent 缓冲有效。
        virtual MappedRange mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes) = 0;
        virtual void unmapBuffer(BufferHandle buffer) = 0;

        /// 对非 Coherent 映射，写入后必须调用以保证 GPU 可见。
        virtual void flushMappedRange(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes) = 0;

        // ---------- 纹理与采样器 ----------

        virtual TextureHandle createTexture(const TextureDesc& desc) = 0;
        virtual void destroyTexture(TextureHandle texture) = 0;

        virtual RhiResult writeTexture(TextureHandle texture, uint32_t mipLevel, const Rect2D& region,
                                       const void* data, uint64_t sizeBytes) = 0;

        virtual SamplerHandle createSampler(const SamplerDesc& desc) = 0;
        virtual void destroySampler(SamplerHandle sampler) = 0;

        // ---------- 绑定组 ----------

        virtual BindGroupHandle createBindGroup(const BindGroupDesc& desc) = 0;
        virtual void destroyBindGroup(BindGroupHandle group) = 0;

        // ---------- 帧与提交 ----------

        /**
         * @brief 取得本帧的命令记录器
         *
         * 返回的指针由设备拥有，仅在本帧内有效，调用方不得保存。
         * 每帧必须成对调用 beginFrame / submitFrame。
         */
        virtual ICommandList* beginFrame(ISurface* surface) = 0;

        /**
         * @brief 提交本帧命令
         *
         * 不执行 present；呈现由 ISurface::present 完成，
         * 使呈现时机与命令提交解耦（多窗口时可批量提交后统一呈现）。
         */
        virtual RhiResult submitFrame() = 0;

        /// 阻塞直到 GPU 完成全部已提交工作。仅用于关闭与资源销毁前。
        virtual void waitIdle() = 0;

        // ---------- 读回 ----------

        /**
         * @brief 从纹理同步读回像素
         *
         * 像素原点统一为左上角（与 CPU 图像约定一致）。
         * GL 后端负责内部翻转，不再把 GL 的左下原点泄漏给上层。
         */
        virtual RhiResult readTexture(TextureHandle texture, const Rect2D& region, void* outPixels,
                                      uint64_t bufferSize, uint32_t* outRowPitch) = 0;

        // ---------- 统计 ----------

        virtual uint64_t gpuMemoryUsageBytes() const = 0;
    };

    /**
     * @brief 创建设备
     *
     * 后端不可用时返回 nullptr 并通过 desc.logCallback 报告原因。
     * 不做静默回退——旧版在后端缺失时悄悄换成 Null 后端，
     * 结果是画面全黑而调用方拿不到任何错误。
     */
    IGpuDevice* createDevice(const DeviceDesc& desc);

    /// 销毁设备。调用前其所有 ISurface 必须已销毁。
    void destroyDevice(IGpuDevice* device);

    /// 查询某后端在当前构建与当前机器上是否可用。
    bool isBackendAvailable(BackendKind backend);

    /// 返回当前平台的推荐后端（macOS→Metal，Windows/Linux→Vulkan，均不可用时→OpenGL）。
    BackendKind preferredBackend();

    const char* backendName(BackendKind backend);
    const char* resultName(RhiResult result);

}  // namespace Render::RHI
