/**
 * @file render_c_api_device.cpp
 * @brief 设备生命周期、视图状态、统计查询、屏幕文本
 *
 * 从 render_c_api.cpp 拆分而来，包含：
 * - 设备创建/销毁/调整大小
 * - 2D/3D 视图设置
 * - 渲染统计与查询
 * - 屏幕字体与文本
 *
 * 日志策略：生产环境仅输出 SY_DEBUGF 级别日志，
 * SY_INFOF 级别的冗余信息已移除以减少性能开销。
 * 警告和错误仍通过 SY_WARNF/SY_ERRORF 输出。
 */
#include "render_c_api_internal.h"

#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

using namespace render;

extern "C"
{
    // ==================== 设备生命周期 ====================

    /**
     * @brief 统一初始化所有渲染模块
     *
     * 返回 false 时已打印错误日志，调用方负责清理已分配的资源。
     */
    static bool initModules(RenderDevice* dev, const DeviceDesc* desc)
    {
        // 初始化顺序：基础模块 -> 高级模块 -> 依赖 PSM 的模块 -> 特殊模块
        // Null backend: 仅分配内部数据结构，不创建 GPU 资源

        if (!dev->world2D.initialize())
        {
            return false;
        }

        if (!dev->batchQueue.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->overlayQueue.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->pipelineStateManager.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->commandEncoder.initialize(dev->rhiDevice))
        {
            return false;
        }

        // Phase 7: 将 PSM 注入 CommandEncoder，使其复用管线缓存
        dev->commandEncoder.setPipelineStateManager(&dev->pipelineStateManager);

        if (!dev->renderGraph.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->drawBatcher.initialize(dev->rhiDevice))
        {
            return false;
        }

        dev->commandEncoder.setDrawBatcher(&dev->drawBatcher);

        // PEM 容量：1<<20 = 1048576 个图元槽位
        // SSBO 占用：entityData 64MB + visibility 4MB + indirect 16MB ≈ 84MB GPU
        // 覆盖大型 DXF 装配图 / 地理图场景（原 65536 太小，普通工程图也会触顶）
        if (!dev->persistentEntityManager.initialize(dev->rhiDevice, 1u << 20))
        {
            return false;
        }

        if (!dev->meshManager.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->world3D.initialize())
        {
            return false;
        }

        if (!dev->textAtlas.initialize(dev->rhiDevice))
        {
            return false;
        }

        if (!dev->screenTextRenderer.initialize(dev->rhiDevice))
        {
            return false;
        }

        // SceneEnv 需要 shader，仅在 OpenGL/Vulkan 后端初始化
        // Null backend 不创建 GPU 管线，跳过 SceneEnv
        // Metal backend 使用 .metal shaders，SceneEnv 初始化在 Metal backend 中处理
        if (desc->backend == BackendType::OpenGL || desc->backend == BackendType::Vulkan)
        {
            if (!dev->sceneEnv.initialize(dev->rhiDevice))
            {
                return false;
            }

            if (!dev->bitmapRenderer.initialize(dev->rhiDevice))
            {
                return false;
            }
        }
        else
        {
            SY_DEBUGF("renderCreateDevice: Null backend - skipping SceneEnv/Bitmap initialization");
        }

        return true;
    }

    RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc)
    {
        if (!desc)
        {
            return nullptr;
        }

    #ifdef _DEBUG
        // 验证 DeviceDesc 结构在当前编译器下的布局已知，
        // 防止跨 DLL 时结构体大小/排列差异导致的兼容性问题。
        // 生产构建中可启用 STATIC_ASSERT(sizeof(DeviceDesc) == 32);
    #endif

        auto* dev = new RenderDevice();
        // 关键：显式零初始化关键成员，防止未初始化的垃圾值导致 GPU 驱动 crash
        // 特别是 RHI 句柄和指针，必须显式置空
        dev->rhiDevice = nullptr;
        dev->world2D.initialize();  // 确保 world2D 处于干净状态
        dev->lastWorld2DGeneration = 0;
        dev->visibleIndices.clear();
        dev->cameraCenter[0] = 0.0f;
        dev->cameraCenter[1] = 0.0f;
        dev->clearColor[0] = 0.0f;
        dev->clearColor[1] = 0.0f;
        dev->clearColor[2] = 0.0f;
        dev->clearColor[3] = 1.0f;
        dev->viewMode = ViewMode::Mode2D;

        // 根据后端类型创建 RHI 设备
        switch (desc->backend)
        {
        case BackendType::OpenGL:
            dev->rhiDevice = rhi::createGLDevice();
            break;
        case BackendType::Null:
            // Null backend: no GPU operations, for testing only
            dev->rhiDevice = rhi::createNullDevice();
            break;
        case BackendType::Vulkan:
#ifdef RENDERX_HAS_VULKAN
            // Vulkan backend: cross-platform GPU backend
            dev->rhiDevice = rhi::createVulkanDevice();
#else
            delete dev;
            SY_ERRORF("renderCreateDevice: Vulkan backend not compiled (RENDERX_HAS_VULKAN not defined)");
            return nullptr;
#endif
            break;
        case BackendType::Metal:
#ifdef RENDERX_HAS_METAL
            dev->rhiDevice = rhi::createMetalDevice();
#else
            delete dev;
            SY_ERRORF("renderCreateDevice: Metal backend not compiled (RENDERX_HAS_METAL not defined)");
            return nullptr;
#endif
            break;
        default:
            delete dev;
            SY_ERRORF("renderCreateDevice: Invalid backend type %u", static_cast<uint32_t>(desc->backend));
            return nullptr;
        }

        if (!dev->rhiDevice)
        {
            delete dev;
            return nullptr;
        }

        // 初始化 RHI 设备
        if (!dev->rhiDevice->initialize(desc->nativeWindowHandle, desc->width, desc->height))
        {
            delete dev;
            return nullptr;
        }

        // 仅 OpenGL/Vulkan 后端需要加载 shader 和字体文件
        // Null backend 和 Metal backend（macOS）不执行任何 GPU 操作/使用 Metal Shading Language
        // M1 改动：shader 初始化通过 RenderRuntime 管理，替代全局静态变量
        if (desc->backend == BackendType::OpenGL || desc->backend == BackendType::Vulkan)
        {
            // 初始化 RenderRuntime（进程级共享资源）
            std::string shaderDir;
#ifdef _WIN32
            wchar_t path[MAX_PATH];
            GetModuleFileNameW(nullptr, path, MAX_PATH);
            std::filesystem::path fsPath(path);
            shaderDir = fsPath.parent_path().string();
#elif defined(__APPLE__)
            char path[PATH_MAX];
            uint32_t size = sizeof(path);
            if (_NSGetExecutablePath(path, &size) == 0)
            {
                std::filesystem::path fsPath(path);
                shaderDir = fsPath.parent_path().string();
            }
            else
            {
                shaderDir = "./";
            }
#else
            std::filesystem::path fsPath("/proc/self/exe");
            if (std::filesystem::exists(fsPath))
            {
                shaderDir = std::filesystem::canonical(fsPath).parent_path().string();
            }
            else
            {
                shaderDir = "./";
            }
#endif
            // 通过 RenderRuntime 初始化 shader，替代全局静态 shader::initialize()
            RenderRuntime::instance().initialize(shaderDir, desc->backend);

            // 自动加载默认屏幕字体（从 exe 所在目录）
            {
                auto fontPath = std::filesystem::path(shaderDir) / "default_screen_font.ttf";
                if (std::filesystem::exists(fontPath))
                {
                    std::ifstream ifs(fontPath, std::ios::binary | std::ios::ate);
                    auto fileSize = ifs.tellg();
                    if (fileSize > 0)
                    {
                        std::vector<uint8_t> buf(static_cast<size_t>(fileSize));
                        ifs.seekg(0);
                        ifs.read(reinterpret_cast<char*>(buf.data()), fileSize);

                        dev->screenTextRenderer.loadFont(buf.data(), static_cast<uint32_t>(buf.size()), 18.0f);

                        SY_DEBUGF("renderCreateDevice: default screen font loaded (%.1f KB)", buf.size() / 1024.0f);
                    }
                }
                else
                {
                    SY_WARNF("renderCreateDevice: default font not found: %s", fontPath.string().c_str());
                }
            }
        }
        else if (desc->backend == BackendType::Metal)
        {
            // Metal backend: shaders are compiled from .metal files, no separate loading needed
            SY_DEBUGF("renderCreateDevice: Metal backend - using embedded shaders");
        }
        else
        {
            SY_DEBUGF("renderCreateDevice: Null backend - skipping shader/font initialization");
        }

        // 初始化所有渲染模块
        // 失败时自动清理已分配资源
        if (!initModules(dev, desc))
        {
            renderDestroyDevice(dev);
            return nullptr;
        }

        dev->initialized = true;
        return dev;
    }

    /**
     * @brief 销毁渲染设备
     *
     * 按逆序关闭所有渲染模块，并释放内存。
     *
     * @param dev 渲染设备指针
     */
    RENDER_API void renderDestroyDevice(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }

        // 按逆序关闭渲染模块
        dev->bitmapRenderer.shutdown();
        dev->sceneEnv.shutdown();
        dev->textAtlas.shutdown();
        dev->screenTextRenderer.shutdown();
        dev->meshManager.shutdown();
        dev->persistentEntityManager.shutdown();  // Phase 9
        dev->drawBatcher.shutdown();              // Phase 8
        dev->pipelineStateManager.shutdown();
        dev->renderGraph.shutdown();
        dev->commandEncoder.shutdown();
        dev->overlayQueue.shutdown();
        dev->batchQueue.shutdown();
        dev->world2D.shutdown();

        // 关闭并删除 RHI 设备
        if (dev->rhiDevice)
        {
            dev->rhiDevice->shutdown();
            delete dev->rhiDevice;
        }

        delete dev;
    }

    /**
     * @brief 调整渲染窗口大小
     *
     * @param dev 渲染设备指针
     * @param width 新宽度
     * @param height 新高度
     */
    RENDER_API void renderResize(RenderDevice* dev, uint32_t width, uint32_t height)
    {
        if (!dev || !dev->rhiDevice)
        {
            return;
        }
        dev->rhiDevice->resize(width, height);
        dev->viewportWidth = width;
        dev->viewportHeight = height;
    }

    // ==================== 视图与渲染状态 ====================

    /**
     * @brief 设置2D视图参数
     *
     * @param dev 渲染设备指针
     * @param viewMatrix 3x3视图矩阵
     * @param viewWidth 视图宽度
     * @param viewHeight 视图高度
     */
    RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9], float viewWidth, float viewHeight)
    {
        if (!dev)
        {
            return;
        }
        std::memcpy(dev->view2D.viewMatrix, viewMatrix, 9 * sizeof(float));
        dev->view2D.viewWidth = viewWidth;
        dev->view2D.viewHeight = viewHeight;

        // World2D 顶点着色器采用 camera-relative 渲染：
        // 先减去相机中心，再乘以只保留缩放项的 viewMatrix。
        // 这里根据 2D 正交相机矩阵自动反推出相机中心，避免调用方忘记同步
        // renderSetCameraCenter() 时，在高倍率缩放或大坐标 DXF 场景下出现
        // 线段断裂、虚线化或短暂消失的问题。
        const float scaleX = viewMatrix[0];
        const float scaleY = viewMatrix[4];
        if (std::abs(scaleX) > 1e-12f && std::abs(scaleY) > 1e-12f && std::isfinite(scaleX) && std::isfinite(scaleY) &&
            std::isfinite(viewMatrix[6]) && std::isfinite(viewMatrix[7]))
        {
            dev->cameraCenter[0] = -static_cast<double>(viewMatrix[6]) / static_cast<double>(scaleX);
            dev->cameraCenter[1] = -static_cast<double>(viewMatrix[7]) / static_cast<double>(scaleY);
        }
    }

    /**
     * @brief 设置3D视图参数
     *
     * @param dev 渲染设备指针
     * @param viewMatrix 4x4视图矩阵
     * @param projMatrix 4x4投影矩阵
     */
    RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16], const float projMatrix[16])
    {
        if (!dev)
        {
            return;
        }
        std::memcpy(dev->view3D.viewMatrix, viewMatrix, 16 * sizeof(float));
        std::memcpy(dev->view3D.projMatrix, projMatrix, 16 * sizeof(float));
    }

    RENDER_API void renderSetViewMode(RenderDevice* dev, ViewMode mode)
    {
        if (!dev)
        {
            return;
        }
        if (dev->viewMode != mode)
        {
            dev->viewMode = mode;
            SY_DEBUGF("renderSetViewMode: switched to %s mode", mode == ViewMode::Mode2D ? "2D" : "3D");
        }
    }

    RENDER_API void renderSetClearColor(RenderDevice* dev, float r, float g, float b, float a)
    {
        if (!dev)
        {
            return;
        }
        dev->clearColor[0] = r;
        dev->clearColor[1] = g;
        dev->clearColor[2] = b;
        dev->clearColor[3] = a;
        if (dev->rhiDevice)
        {
            dev->rhiDevice->setClearColor(r, g, b, a);
        }
    }

    // ==================== 统计与查询 ====================

    /**
     * @brief 获取渲染统计信息
     *
     * @param dev 渲染设备指针
     * @param stats 输出统计信息
     */
    RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats)
    {
        if (!dev || !stats)
        {
            return;
        }
        *stats = dev->stats;
    }

    /**
     * @brief 获取图元数量
     *
     * @param dev 渲染设备指针
     * @return 图元数量
     */
    RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev)
    {
        if (!dev)
        {
            return 0;
        }
        return dev->getEntityCount();
    }

    /**
     * @brief 获取GPU内存使用量
     *
     * @param dev 渲染设备指针
     * @return GPU内存使用量（字节）
     */
    RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev)
    {
        if (!dev || !dev->rhiDevice)
        {
            return 0;
        }
        return dev->rhiDevice->getGPUMemoryUsage();
    }

    /**
     * @brief 获取原生渲染上下文
     *
     * 返回底层图形API的上下文指针（如OpenGL的GLContext）。
     *
     * @param dev 渲染设备指针
     * @return 原生上下文指针
     */
    RENDER_API void* renderGetNativeContext(RenderDevice* dev)
    {
        if (!dev || !dev->rhiDevice)
        {
            return nullptr;
        }
        return dev->rhiDevice->getNativeContext();
    }

    /**
     * @brief 读取当前帧缓冲区像素数据
     */
    RENDER_API int renderReadPixels(
        RenderDevice* dev, uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* outPixels, uint32_t* outRowPitch)
    {
        if (!dev || !dev->rhiDevice)
        {
            return 0;
        }
        return dev->rhiDevice->readPixels(x, y, width, height, outPixels, outRowPitch);
    }

    // ==================== 屏幕文本 ====================

    /**
     * @brief 加载屏幕文本渲染器的字体（可选覆盖，默认字体由 renderCreateDevice 自动加载）
     */
    RENDER_API void renderLoadScreenFont(RenderDevice* dev, const void* fontData, uint32_t dataSize, float pixelHeight)
    {
        if (!dev || !fontData || dataSize == 0)
        {
            return;
        }
        dev->screenTextRenderer.loadFont(fontData, dataSize, pixelHeight);
    }

    /**
     * @brief 暂存屏幕空间文本（在 renderFrame 末尾统一渲染）
     */
    RENDER_API void renderSetScreenTexts(RenderDevice* dev, const ScreenTextItem* items, uint32_t count)
    {
        if (!dev)
        {
            return;
        }
        dev->submitScreenTexts(items, count);
    }
}  // extern "C"