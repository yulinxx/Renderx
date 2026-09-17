/**
 * @file rhiFactory.cpp
 * @brief RHI 公共入口：设备创建、后端可用性查询、枚举名字
 *
 * 明确的设计取向：**不做静默回退**。
 *
 * 旧实现在 Vulkan/Metal 不可用时悄悄换成 Null 后端，调用方拿到一个
 * 「创建成功」的设备，然后画面全黑，没有任何错误可查。现在
 * createDevice 只创建调用方要求的后端，失败就返回 nullptr 并通过
 * logCallback 说明原因；BackendKind::Auto 是唯一允许自动选择的取值，
 * 且它的选择顺序有明确定义（见 preferredBackend）。
 */

#include "rhi/rhiBackendFactory.h"
#include "rhi/rhiLog.h"

namespace Render::RHI
{
    const char* backendName(BackendKind backend)
    {
        switch (backend)
        {
        case BackendKind::Null:
            return "Null";
        case BackendKind::OpenGL:
            return "OpenGL";
        case BackendKind::Metal:
            return "Metal";
        case BackendKind::Vulkan:
            return "Vulkan";
        }
        return "?";
    }

    const char* resultName(RhiResult result)
    {
        switch (result)
        {
        case RhiResult::Ok:
            return "Ok";
        case RhiResult::ErrorUnknown:
            return "ErrorUnknown";
        case RhiResult::ErrorInvalidArgument:
            return "ErrorInvalidArgument";
        case RhiResult::ErrorOutOfMemory:
            return "ErrorOutOfMemory";
        case RhiResult::ErrorDeviceLost:
            return "ErrorDeviceLost";
        case RhiResult::ErrorUnsupported:
            return "ErrorUnsupported";
        case RhiResult::ErrorNotInitialized:
            return "ErrorNotInitialized";
        case RhiResult::ErrorSurfaceLost:
            return "ErrorSurfaceLost";
        case RhiResult::ErrorSwapchainOutOfDate:
            return "ErrorSwapchainOutOfDate";
        case RhiResult::ErrorShaderCompilation:
            return "ErrorShaderCompilation";
        case RhiResult::ErrorResourceCreation:
            return "ErrorResourceCreation";
        }
        return "?";
    }

    bool isBackendAvailable(BackendKind backend)
    {
        switch (backend)
        {
        case BackendKind::Null:
            // Null 后端不依赖任何外部条件
            return true;
        case BackendKind::OpenGL:
            // 是否真的可用取决于「调用时是否有当前上下文」，
            // 这在此处无法判定；编译进来即视为可用，
            // 实际失败在 createDevice 里报告（GL 函数加载失败）。
            return true;
        case BackendKind::Metal:
#if defined(__APPLE__)
            // 与 GL 同一策略：后端编译进来即视为可用，真正的失败
            // （本机没有 Metal 设备）在 createDevice 里报告。
            return true;
#else
            return false;
#endif
        case BackendKind::Vulkan:
            // Phase 8 落地前明确返回不可用，
            // 而不是「有个空壳实现，跑起来才发现什么都没画」。
            return false;
        }
        return false;
    }

    BackendKind preferredBackend()
    {
        // 自动选择只考虑**已经能承担完整渲染**的后端。
        //
        // Metal 后端（设备/表面/资源 + 管线与绘制）在 Apple 平台上已经可用，
        // 但产品路径不会落到这里的自动分支：宿主视口通过编译期开关
        // SY_ENABLE_METAL_VIEWPORT 把具体后端显式传给 Runtime（见根 CMakeLists），
        // Runtime 解析 Auto 之前就已确定后端。
        // 这里保持 OpenGL 作为 Auto 的结果，是为了让「没显式指定后端」的调用方
        // （测试、工具、无头路径）行为完全不变。
        if (isBackendAvailable(BackendKind::Vulkan))
        {
            return BackendKind::Vulkan;
        }
        return BackendKind::OpenGL;
    }

    IGpuDevice* createDevice(const DeviceDesc& desc)
    {
        RhiLogger logger(desc.logCallback, desc.logUserData);

        // 注意：内部 RHI 的 BackendKind 没有 Auto。
        // 「自动选择」属于公共 ABI（renderx.h 的 Backend::Auto），
        // 由 Runtime 调 preferredBackend() 解析后再传具体后端进来——
        // RHI 层不做隐式决策，这样日志里永远能看到实际用的是哪个后端。
        const BackendKind backend = desc.backend;

        switch (backend)
        {
        case BackendKind::Null:
            return createNullDevice(desc);
        case BackendKind::OpenGL:
            return createGlDevice(desc);
        case BackendKind::Metal:
#if defined(__APPLE__)
            return createMetalDevice(desc);
#else
            logger.error("[rhi] Metal backend is only compiled on Apple platforms, no Metal backend on this platform. "
                         "No silent fallback: please explicitly select OpenGL.");  // Metal 后端只在 Apple 平台编译
            return nullptr;
#endif
        case BackendKind::Vulkan:
            logger.error(
                "[rhi] Vulkan backend not yet implemented (Phase 8). "
                "No silent fallback: please explicitly select OpenGL, or wait for Vulkan.");  // Vulkan 后端尚未实现
            return nullptr;
        }

        logger.error("[rhi] createDevice: 未知的 backend 取值 %d", static_cast<int>(backend));
        return nullptr;
    }

    void destroyDevice(IGpuDevice* device)
    {
        // 析构里会检查表面与资源是否已释放，并对泄漏记 Warn/Error。
        delete device;
    }
}  // namespace Render::RHI