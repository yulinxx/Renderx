/**
 * @file rhiSurface.h
 * @brief 窗口表面与交换链抽象
 *
 * 旧版 RHI 完全没有这一层：IDevice::initialize 直接吃一个 `void* nativeWindow`
 * （注释写「如 HWND」），present() 是空实现，buffer swap 与 makeCurrent 全靠宿主
 * Qt 完成。这使得：
 * - 设备与窗口一对一绑死，多窗口只能靠多设备，无法共享 GPU 资源
 * - Vulkan/Metal 的 swapchain / drawable 生命周期无处安放
 * - 上下文 current 性没有任何契约表达
 *
 * 本文件把「窗口」与「设备」正交化：
 *   IGpuDevice  —— GPU 与资源，进程内可被多个窗口共享
 *   ISurface    —— 一个窗口的原生表面 + 交换链，每窗口一个
 *
 * 线程契约（全 RHI 统一，实现方必须遵守）：
 * - 同一个 ISurface 的 acquire/present 必须在同一线程调用。
 * - IGpuDevice 的资源创建/销毁在 Capabilities 未声明 threadSafeResourceCreation
 *   时也必须串行；当前所有后端均为串行。
 * - GL 后端在 acquireNextImage 内部完成上下文 makeCurrent，
 *   调用方不再需要（也不应该）自己 makeCurrent。
 */
#pragma once

#include "rhiCore.h"

namespace Render::RHI
{

    /**
     * @brief 原生窗口句柄载荷
     *
     * 不使用裸 void*：各平台需要的信息量不同，Vulkan 在 Linux 上需要
     * (display, window) 两个值，Metal 需要 NSView*，GL 需要既有上下文。
     * 用带 tag 的联合体表达，避免调用方猜测语义。
     */
    struct NativeWindow
    {
        enum class Kind : uint8_t
        {
            None = 0,
            Win32Hwnd,     ///< handleA = HWND
            CocoaNsView,   ///< handleA = NSView*（Metal 会在其上挂 CAMetalLayer）
            XlibWindow,    ///< handleA = Display*, handleB = Window
            WaylandSurface,///< handleA = wl_display*, handleB = wl_surface*
            /// 宿主已自行创建并管理 GL 上下文（Qt QOpenGLWidget 场景）。
            /// 此时 GL 后端不创建上下文，仅记录，并要求宿主在
            /// beginFrame 前保证上下文 current。
            ForeignGlContext,
        };

        Kind kind = Kind::None;
        void* handleA = nullptr;
        void* handleB = nullptr;
    };

    /// 交换链呈现模式
    enum class PresentMode : uint8_t
    {
        Immediate = 0,  ///< 不等待垂直同步
        Fifo,           ///< 垂直同步，保证不撕裂（各后端必须支持）
        Mailbox,        ///< 三缓冲低延迟，不支持时回退到 Fifo
    };

    struct SurfaceDesc
    {
        NativeWindow window{};
        Extent2D initialExtent{};
        Format preferredColorFormat = Format::BGRA8Unorm;
        Format depthFormat = Format::D32Float;  ///< Unknown 表示不需要深度附件
        PresentMode presentMode = PresentMode::Fifo;
        const char* debugName = nullptr;
    };

    /**
     * @brief 一个窗口的可呈现表面
     *
     * 生命周期由 IGpuDevice::createSurface / destroySurface 管理，
     * 且必须在其所属 device 之前销毁。
     */
    class ISurface
    {
    public:
        virtual ~ISurface() = default;

        /**
         * @brief 获取下一个可渲染的后备缓冲
         *
         * GL 后端在此完成 makeCurrent。
         * 返回 ErrorSwapchainOutOfDate 时调用方应调用 resize 后重试本帧。
         */
        virtual RhiResult acquireNextImage() = 0;

        /**
         * @brief 提交并呈现当前后备缓冲
         *
         * GL 后端在此执行 swapBuffers，Metal 执行 presentDrawable，
         * Vulkan 执行 vkQueuePresentKHR。
         */
        virtual RhiResult present() = 0;

        /// 重建交换链。尺寸为 0 时表示窗口最小化，实现应安全跳过。
        virtual RhiResult resize(Extent2D extent) = 0;

        virtual Extent2D extent() const = 0;
        virtual Format colorFormat() const = 0;
        virtual Format depthFormat() const = 0;

        /// 当前后备缓冲对应的颜色纹理。可用于 RenderPassBeginDesc 显式引用。
        virtual TextureHandle currentColorTexture() const = 0;
        /// 深度附件纹理；depthFormat 为 Unknown 时返回无效句柄。
        virtual TextureHandle depthTexture() const = 0;
    };

}  // namespace Render::RHI
