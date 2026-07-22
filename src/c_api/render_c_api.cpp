/**
 * @file render_c_api.cpp
 * @brief C API 实现文件
 *
 * 提供 Renderx 渲染框架的 C 语言 API，作为外部应用程序与渲染核心之间的桥梁。
 * 实现了设备创建/销毁、实体管理、视图设置、渲染帧等核心功能。
 */
#include "render/render.h"
#include "render/render_types.h"
#include "core/render_world.h"
#include "core/batch_queue.h"
#include "core/overlay_queue.h"
#include "core/mesh_manager.h"
#include "core/text_atlas.h"
#include "core/scene_env.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gl.h"
#include "shader/shaders.h"

#include <cstring>
#include <string>
#include <vector>
#include <cassert>
#include <vector>
#include <cmath>
#include <filesystem>

#include "Log/SyLogger.h"

namespace render
{
    /**
     * @brief 渲染设备内部结构
     *
     * 包含所有渲染模块的实例和状态信息。
     */
    struct RenderDevice
    {
        /// RHI 设备接口
        rhi::IDevice* rhiDevice = nullptr;

        /// 2D 渲染世界（实体管理和可见性查询）
        core::RenderWorld    world2D;
        /// 批处理队列（2D 实体批处理渲染）
        core::BatchQueue     batchQueue;
        /// 叠加层队列（UI 元素渲染）
        core::OverlayQueue   overlayQueue;
        /// 网格管理器（3D 网格实例化渲染）
        core::MeshManager    meshManager;
        /// 文本贴图管理器（文本渲染）
        core::TextAtlas      textAtlas;
        /// 场景环境渲染器（网格背景等）
        core::SceneEnv       sceneEnv;

        /// 视图模式（2D/3D）
        ViewMode             viewMode = ViewMode::Mode2D;

        /// 2D 视图描述
        ViewDesc2D           view2D = {};
        /// 3D 视图描述
        ViewDesc3D           view3D = {};
        /// 叠加层数据
        OverlayData          overlay = {};
        /// 渲染统计信息
        RenderStats          stats = {};
        /// 是否已初始化
        bool                 initialized = false;

        /// 可见实体索引缓存
        std::vector<uint32_t> visibleIndices;

        /// 帧计数器（用于日志节流，每60帧输出一次统计）
        uint32_t             frameCounter = 0;

        /// 发射模式暂存的文本项（在 renderFrame 中渲染）
        struct PendingText
        {
            TextItem    item;
            std::string textStorage;
        };
        std::vector<PendingText> pendingTextItems;
    };
}

using namespace render;

extern "C" {
    /**
     * @brief 创建渲染设备
     *
     * 根据指定的后端类型创建渲染设备，并初始化所有渲染模块。
     *
     * @param desc 设备描述符
     * @return 渲染设备指针，失败返回nullptr
     */
    RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc)
    {
        if (!desc) return nullptr;

        auto* dev = new RenderDevice();

        // 根据后端类型创建 RHI 设备
        switch (desc->backend)
        {
            case BackendType::OpenGL:
                dev->rhiDevice = rhi::createGLDevice();
                break;
            default:
                delete dev;
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

        std::string shaderDir;
#ifdef _WIN32
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::filesystem::path fsPath(path);
        shaderDir = fsPath.parent_path().string();
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
        shader::initialize(shaderDir);

        // 初始化所有渲染模块
        dev->world2D.initialize();
        dev->batchQueue.initialize(dev->rhiDevice);
        dev->overlayQueue.initialize(dev->rhiDevice);
        dev->meshManager.initialize(dev->rhiDevice);
        dev->textAtlas.initialize(dev->rhiDevice);
        dev->sceneEnv.initialize(dev->rhiDevice);

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
        if (!dev) return;

        // 按逆序关闭渲染模块
        dev->sceneEnv.shutdown();
        dev->textAtlas.shutdown();
        dev->meshManager.shutdown();
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
        if (!dev || !dev->rhiDevice) return;
        dev->rhiDevice->resize(width, height);
    }

    /**
     * @brief 添加实体到渲染世界
     *
     * @param dev 渲染设备指针
     * @param id 实体ID
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param type 图元类型
     * @param materialIdx 材质索引
     * @return 是否成功（1表示成功，0表示失败）
     */
    RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        PrimitiveType type, uint16_t materialIdx)
    {
        if (!dev) return 0;
        dev->world2D.addEntity(id, vertices, vertexCount, type, materialIdx);
        return 1;
    }

    /**
     * @brief 修改实体的顶点数据
     *
     * @param dev 渲染设备指针
     * @param id 实体ID
     * @param vertices 新的顶点数据
     * @param vertexCount 顶点数量
     * @param materialIdx 材质索引
     */
    RENDER_API void renderModifyEntity(RenderDevice* dev, EntityId id,
        const VertexP3C3* vertices, uint32_t vertexCount,
        uint16_t materialIdx)
    {
        if (!dev) return;
        dev->world2D.modifyEntity(id, vertices, vertexCount, materialIdx);
    }

    /**
     * @brief 从渲染世界移除实体
     *
     * @param dev 渲染设备指针
     * @param id 实体ID
     */
    RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id)
    {
        if (!dev) return;
        dev->world2D.removeEntity(id);
    }

    /**
     * @brief 设置实体可见性
     *
     * @param dev 渲染设备指针
     * @param id 实体ID
     * @param visible 是否可见（非0表示可见）
     */
    RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible)
    {
        if (!dev) return;
        dev->world2D.setEntityVisibility(id, visible != 0);
    }

    /**
     * @brief 批量应用实体更新
     *
     * 从更新数据包中解析并应用一系列实体操作（添加、修改、删除）。
     *
     * @param dev 渲染设备指针
     * @param packet 更新数据包指针
     * @param packetSize 数据包大小
     */
    RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize)
    {
        if (!dev || !packet) return;

        const uint8_t* ptr = static_cast<const uint8_t*>(packet);
        const uint8_t* end = ptr + packetSize;

        // 解析更新数量（前4字节）
        uint32_t updateCount;
        std::memcpy(&updateCount, ptr, 4);
        ptr += 8; // 跳过8字节（4字节计数 + 4字节对齐）

        // 逐个解析并应用更新
        for (uint32_t i = 0; i < updateCount && ptr < end; ++i)
        {
            EntityUpdate upd;
            std::memcpy(&upd, ptr, sizeof(EntityUpdate));
            ptr += sizeof(EntityUpdate);

            // 获取顶点数据
            const VertexP3C3* verts = reinterpret_cast<const VertexP3C3*>(ptr);
            ptr += upd.vertexCount * sizeof(VertexP3C3);

            // 根据操作类型执行相应操作
            switch (upd.op)
            {
                case UpdateOp::Add:
                    dev->world2D.addEntity(upd.entityId, verts, upd.vertexCount,
                        static_cast<PrimitiveType>(upd.primitiveType), upd.materialIndex);
                    break;
                case UpdateOp::Modify:
                    dev->world2D.modifyEntity(upd.entityId, verts, upd.vertexCount, upd.materialIndex);
                    break;
                case UpdateOp::Remove:
                    dev->world2D.removeEntity(upd.entityId);
                    break;
            }
        }
    }

    /**
     * @brief 注册3D网格
     *
     * @param dev 渲染设备指针
     * @param positions 顶点位置数组（每点3个float）
     * @param normals 顶点法线数组（每点3个float）
     * @param indices 索引数组
     * @param vertexCount 顶点数量
     * @param indexCount 索引数量
     * @return 网格ID
     */
    RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
        const float* positions, const float* normals,
        const uint32_t* indices,
        uint32_t vertexCount, uint32_t indexCount)
    {
        if (!dev) return INVALID_MESH_ID;
        return dev->meshManager.registerMesh(positions, normals, indices, vertexCount, indexCount);
    }

    /**
     * @brief 注销3D网格
     *
     * @param dev 渲染设备指针
     * @param mesh 网格ID
     */
    RENDER_API void renderUnregisterMesh(RenderDevice* dev, MeshId mesh)
    {
        if (!dev) return;
        dev->meshManager.unregisterMesh(mesh);
    }

    /**
     * @brief 添加网格实例
     *
     * @param dev 渲染设备指针
     * @param mesh 网格ID
     * @param modelMatrix 4x4模型矩阵
     * @param materialIdx 材质索引
     * @return 实例ID
     */
    RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
        const float modelMatrix[16], uint32_t materialIdx,
        const float color[4])
    {
        if (!dev) return UINT32_MAX;
        return dev->meshManager.addInstance(mesh, modelMatrix, materialIdx, color);
    }

    /**
     * @brief 修改网格实例的变换
     *
     * @param dev 渲染设备指针
     * @param instanceId 实例ID
     * @param modelMatrix 新的4x4模型矩阵
     */
    RENDER_API void renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
        const float modelMatrix[16])
    {
        if (!dev) return;
        dev->meshManager.modifyInstance(instanceId, modelMatrix);
    }

    /**
     * @brief 移除网格实例
     *
     * @param dev 渲染设备指针
     * @param instanceId 实例ID
     */
    RENDER_API void renderRemoveInstance(RenderDevice* dev, uint32_t instanceId)
    {
        if (!dev) return;
        dev->meshManager.removeInstance(instanceId);
    }

    /**
     * @brief 添加材质
     *
     * @param dev 渲染设备指针
     * @param desc 材质描述符
     * @return 材质索引
     */
    RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc)
    {
        if (!dev) return 0;
        return dev->world2D.addMaterial(desc);
    }

    /**
     * @brief 更新材质
     *
     * @param dev 渲染设备指针
     * @param idx 材质索引
     * @param desc 材质描述符
     */
    RENDER_API void renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc)
    {
        if (!dev) return;
        dev->world2D.updateMaterial(idx, desc);
    }

    /**
     * @brief 设置2D视图参数
     *
     * @param dev 渲染设备指针
     * @param viewMatrix 3x3视图矩阵
     * @param viewWidth 视图宽度
     * @param viewHeight 视图高度
     */
    RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9],
        float viewWidth, float viewHeight)
    {
        if (!dev) return;
        std::memcpy(dev->view2D.viewMatrix, viewMatrix, 9 * sizeof(float));
        dev->view2D.viewWidth = viewWidth;
        dev->view2D.viewHeight = viewHeight;
    }

    /**
     * @brief 设置3D视图参数
     *
     * @param dev 渲染设备指针
     * @param viewMatrix 4x4视图矩阵
     * @param projMatrix 4x4投影矩阵
     */
    RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16],
        const float projMatrix[16])
    {
        if (!dev) return;
        std::memcpy(dev->view3D.viewMatrix, viewMatrix, 16 * sizeof(float));
        std::memcpy(dev->view3D.projMatrix, projMatrix, 16 * sizeof(float));
    }

    RENDER_API void renderSetViewMode(RenderDevice* dev, ViewMode mode)
    {
        if (!dev) return;
        if (dev->viewMode != mode)
        {
            dev->viewMode = mode;
            SY_INFOF("renderSetViewMode: switched to %s mode",
                mode == ViewMode::Mode2D ? "2D" : "3D");
        }
    }

    /**
     * @brief 设置叠加层数据
     *
     * @param dev 渲染设备指针
     * @param overlay 叠加层数据
     */
    RENDER_API void renderSetOverlay(RenderDevice* dev, const OverlayData* overlay)
    {
        if (!dev || !overlay) return;
        dev->overlay = *overlay;
        dev->overlayQueue.setCrosshair(overlay->crosshairWorld[0], overlay->crosshairWorld[1],
            overlay->crosshairVisible != 0);
        dev->overlayQueue.setSnapIndicator(overlay->snapWorld[0], overlay->snapWorld[1],
            overlay->snapVisible != 0, overlay->snapColor);
    }

    /**
     * @brief 设置要渲染的文本列表
     *
     * @param dev 渲染设备指针
     * @param texts 文本项列表
     */
    RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts)
    {
        if (!dev) return;
        dev->textAtlas.renderText(texts, dev->view2D.viewMatrix, dev->rhiDevice);
    }

    /**
     * @brief 设置预览线
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetPreviewLines(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, uint32_t colorRGBA)
    {
        if (!dev) return;
        dev->overlayQueue.setPreviewLines(vertices, vertexCount, colorRGBA);
    }

    /**
     * @brief 设置控制线
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetControlLines(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, uint32_t colorRGBA)
    {
        if (!dev) return;
        dev->overlayQueue.setControlLines(vertices, vertexCount, colorRGBA);
    }

    /**
     * @brief 设置点标记
     *
     * @param dev 渲染设备指针
     * @param worldPositions 世界坐标数组（每点2个float）
     * @param count 点数量
     * @param markerSize 标记大小（像素）
     * @param fillColor 填充颜色（32位RGBA格式）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    RENDER_API void renderSetPointMarkers(RenderDevice* dev, const float* worldPositions,
        uint32_t count, float markerSize,
        uint32_t fillColor, uint32_t borderColor)
    {
        if (!dev) return;
        dev->overlayQueue.setPointMarkers(worldPositions, count, markerSize, fillColor, borderColor);
    }

    /**
     * @brief 设置选择框
     *
     * @param dev 渲染设备指针
     * @param bbox 边界框
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetSelectionBox(RenderDevice* dev, const BBox2f* bbox, uint32_t colorRGBA)
    {
        if (!dev) return;
        dev->overlayQueue.setSelectionBox(bbox, colorRGBA);
    }

    /**
     * @brief 设置选择手柄
     *
     * @param dev 渲染设备指针
     * @param worldPositions 世界坐标数组（每点2个float）
     * @param count 手柄数量
     * @param handleSize 手柄大小（像素）
     * @param fillColor 填充颜色（32位RGBA格式）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    RENDER_API void renderSetSelectionHandles(RenderDevice* dev, const float* worldPositions,
        uint32_t count, float handleSize,
        uint32_t fillColor, uint32_t borderColor)
    {
        if (!dev) return;
        dev->overlayQueue.setSelectionHandles(worldPositions, count, handleSize, fillColor, borderColor);
    }

    /**
     * @brief 设置场景环境几何数据
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param layerOffsets 各层的顶点偏移数组
     * @param layerCount 层数
     * @param layerColors 各层的颜色数组（RGBA格式）
     * @param layerWidths 各层的线宽数组
     */
    RENDER_API void renderSetSceneEnv(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, const uint32_t* layerOffsets,
        uint32_t layerCount, const uint32_t* layerColors,
        const float* layerWidths)
    {
        if (!dev) return;
        dev->sceneEnv.setGeometry(vertices, vertexCount, layerOffsets, layerCount, layerColors, layerWidths);
    }

    RENDER_API void renderSetSceneEnvEx(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, const uint32_t* layerOffsets,
        uint32_t layerCount, const uint32_t* layerColors,
        const float* layerWidths, const bool* pixelFlags)
    {
        if (!dev) return;
        dev->sceneEnv.setGeometryEx(vertices, vertexCount, layerOffsets, layerCount,
            layerColors, layerWidths, pixelFlags);
    }

    /**
     * @brief 设置位图（预留接口，尚未实现）
     */
    RENDER_API void renderSetBitmap(RenderDevice* dev, const uint8_t* rgba, int32_t w, int32_t h,
        float tlX, float tlY, float trX, float trY,
        float blX, float blY, float brX, float brY)
    {
        (void)dev; (void)rgba; (void)w; (void)h;
        (void)tlX; (void)tlY; (void)trX; (void)trY;
        (void)blX; (void)blY; (void)brX; (void)brY;
    }

    /**
     * @brief 清除位图（预留接口，尚未实现）
     */
    RENDER_API void renderClearBitmap(RenderDevice* dev)
    {
        (void)dev;
    }

    /**
     * @brief 渲染一帧
     *
     * 执行完整的渲染流程：
     * 1. 开始帧，设置渲染状态
     * 2. 更新渲染世界
     * 3. 查询可见实体
     * 4. 渲染场景环境（网格背景）
     * 5. 渲染2D实体批处理
     * 6. 渲染叠加层（十字准星、预览线等）
     * 7. 渲染3D网格实例（如果存在）
     * 8. 结束帧并呈现
     *
     * @param dev 渲染设备指针
     */
    RENDER_API void renderFrame(RenderDevice* dev)
    {
        if (!dev || !dev->rhiDevice)
        {
            SY_ERROR("renderFrame: dev or rhiDevice is null");
            return;
        }

        auto* rhi = dev->rhiDevice;

        rhi->beginFrame();

        if (dev->viewMode == ViewMode::Mode2D)
        {
            rhi->setClearColor(1.0f, 0.98f, 0.86f, 1.0f);
            rhi->enableDepthTest(false);
        }
        else
        {
            rhi->setClearColor(0.12f, 0.14f, 0.20f, 1.0f);
            rhi->enableDepthTest(true);
        }
        rhi->enableBlend(true);

        uint32_t visibleCount = 0;

        if (dev->viewMode == ViewMode::Mode2D)
        {
            dev->world2D.update();

            uint32_t maxVisible = static_cast<uint32_t>(dev->world2D.getEntityCount());

            if (dev->visibleIndices.size() < maxVisible)
            {
                dev->visibleIndices.resize(maxVisible);
            }
            dev->world2D.queryVisible(dev->view2D.viewMatrix, dev->view2D.viewWidth,
                dev->view2D.viewHeight, dev->visibleIndices.data(), &visibleCount, maxVisible);

            if (maxVisible > 0 && visibleCount == 0)
            {
                SY_WARNF("renderFrame: queryVisible returned 0 (total=%u), forcing all entities for debug", maxVisible);
                visibleCount = maxVisible;
                for (uint32_t i = 0; i < maxVisible; ++i)
                    dev->visibleIndices[i] = i;
            }

            dev->sceneEnv.render(rhi, dev->view2D.viewMatrix,
                static_cast<uint32_t>(dev->view2D.viewWidth),
                static_cast<uint32_t>(dev->view2D.viewHeight));

            dev->batchQueue.submit(dev->visibleIndices.data(), visibleCount, dev->world2D);
            dev->batchQueue.render(rhi, dev->view2D.viewMatrix, dev->world2D);

            dev->overlayQueue.render(rhi, dev->view2D.viewMatrix);

            if (!dev->pendingTextItems.empty())
            {
                std::vector<TextItem> items;
                items.reserve(dev->pendingTextItems.size());
                for (auto& pt : dev->pendingTextItems)
                    items.push_back(pt.item);
                TextItemList list;
                list.items = items.data();
                list.count = static_cast<uint32_t>(items.size());
                dev->textAtlas.renderText(&list, dev->view2D.viewMatrix, rhi);
                dev->pendingTextItems.clear();
            }
        }
        else
        {
            if (dev->meshManager.getInstanceCount() > 0)
            {
                dev->meshManager.update();
                dev->meshManager.render(rhi, dev->view3D.viewMatrix, dev->view3D.projMatrix);
            }
        }

        rhi->endFrame();
        rhi->present();

        dev->stats.entityCount = dev->world2D.getEntityCount();
        dev->stats.visibleCount = visibleCount;
        dev->stats.gpuMemoryBytes = rhi->getGPUMemoryUsage();

        ++dev->frameCounter;
        if (dev->frameCounter >= 60)
        {
            SY_INFOF("RenderStats: entities=%u, visible=%u, gpuMemory=%llu bytes",
                dev->stats.entityCount, dev->stats.visibleCount, dev->stats.gpuMemoryBytes);
            dev->frameCounter = 0;
        }
    }

    /**
     * @brief 获取渲染统计信息
     *
     * @param dev 渲染设备指针
     * @param stats 输出统计信息
     */
    RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats)
    {
        if (!dev || !stats) return;
        *stats = dev->stats;
    }

    /**
     * @brief 获取实体数量
     *
     * @param dev 渲染设备指针
     * @return 实体数量
     */
    RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev)
    {
        if (!dev) return 0;
        return dev->world2D.getEntityCount();
    }

    /**
     * @brief 获取GPU内存使用量
     *
     * @param dev 渲染设备指针
     * @return GPU内存使用量（字节）
     */
    RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev)
    {
        if (!dev || !dev->rhiDevice) return 0;
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
        if (!dev || !dev->rhiDevice) return nullptr;
        return dev->rhiDevice->getNativeContext();
    }

    /// 场景模式下的实体ID计数器
    static uint64_t s_entityIdCounter = 1;

    /**
     * @brief 开始场景模式
     *
     * 清除所有旧实体，重置实体ID计数器，准备接收新的场景数据。
     *
     * @param dev 渲染设备指针
     */
    RENDER_API void renderBeginScene(RenderDevice* dev)
    {
        if (!dev)
        {
            SY_ERROR("renderBeginScene: dev is null");
            return;
        }
        dev->world2D.clearAllEntities();
        dev->pendingTextItems.clear();
        s_entityIdCounter = 1;
    }

    /**
     * @brief 将多段线几何数据细分为顶点
     *
     * @param polyline 多段线几何数据
     * @param outVertices 输出顶点数组
     */
    static void tessellatePolyline(const GeometryPolyline* polyline, std::vector<render::VertexP3C3>& outVertices)
    {
        if (!polyline || !polyline->points || polyline->pointCount < 2)
            return;

        float cr = polyline->color[0], cg = polyline->color[1], cb = polyline->color[2];
        outVertices.reserve(polyline->pointCount);
        for (uint32_t i = 0; i < polyline->pointCount; ++i)
        {
            outVertices.push_back({
                static_cast<float>(polyline->points[i].x),
                static_cast<float>(polyline->points[i].y),
                0.0f,
                cr, cg, cb
                });
        }
    }

    /**
     * @brief 将圆形几何数据细分为顶点
     *
     * 使用64段线段近似圆形。
     *
     * @param circle 圆形几何数据
     * @param outVertices 输出顶点数组
     */
    static void tessellateCircle(const GeometryCircle* circle, std::vector<render::VertexP3C3>& outVertices)
    {
        if (!circle || circle->radius <= 0)
            return;

        float cr = circle->color[0], cg = circle->color[1], cb = circle->color[2];
        constexpr int segments = 64;
        outVertices.reserve(segments);
        const double centerX = circle->center.x;
        const double centerY = circle->center.y;
        const double radius = circle->radius;

        for (int i = 0; i < segments; ++i)
        {
            double angle = (2.0 * 3.14159265358979323846 * i) / segments;
            outVertices.push_back({
                static_cast<float>(centerX + radius * std::cos(angle)),
                static_cast<float>(centerY + radius * std::sin(angle)),
                0.0f,
                cr, cg, cb
                });
        }
    }

    /**
     * @brief 将圆弧几何数据细分为顶点
     *
     * 根据圆弧角度范围动态计算细分段数（8-128段）。
     *
     * @param arc 圆弧几何数据
     * @param outVertices 输出顶点数组
     */
    static void tessellateArc(const GeometryArc* arc, std::vector<render::VertexP3C3>& outVertices)
    {
        if (!arc || arc->radius <= 0)
            return;

        float cr = arc->color[0], cg = arc->color[1], cb = arc->color[2];
        double start = arc->startAngle;
        double end = arc->endAngle;
        if (end < start)
            end += 2.0 * 3.14159265358979323846;

        double angleRange = end - start;
        constexpr int minSegments = 8;
        constexpr int maxSegments = 128;
        int segments = static_cast<int>(angleRange * 20.0);
        segments = segments < minSegments ? minSegments : (segments > maxSegments ? maxSegments : segments);

        outVertices.reserve(segments);
        const double centerX = arc->center.x;
        const double centerY = arc->center.y;
        const double radius = arc->radius;

        for (int i = 0; i <= segments; ++i)
        {
            double t = static_cast<double>(i) / segments;
            double angle = start + t * angleRange;
            outVertices.push_back({
                static_cast<float>(centerX + radius * std::cos(angle)),
                static_cast<float>(centerY + radius * std::sin(angle)),
                0.0f,
                cr, cg, cb
                });
        }
    }

    /**
     * @brief 将椭圆几何数据细分为顶点
     *
     * 支持旋转椭圆，根据角度范围动态计算细分段数。
     *
     * @param ellipse 椭圆几何数据
     * @param outVertices 输出顶点数组
     */
    static void tessellateEllipse(const GeometryEllipse* ellipse, std::vector<render::VertexP3C3>& outVertices)
    {
        if (!ellipse || ellipse->radiusX <= 0 || ellipse->radiusY <= 0)
            return;

        constexpr int segments = 64;
        outVertices.reserve(segments);

        double start = ellipse->startAngle;
        double end = ellipse->endAngle;
        if (ellipse->fullEllipse || (start == 0.0 && end == 0.0))
        {
            start = 0.0;
            end = 2.0 * 3.14159265358979323846;
        }

        double angleRange = end - start;
        int actualSegments = static_cast<int>(angleRange / (2.0 * 3.14159265358979323846) * segments);
        actualSegments = actualSegments < 8 ? 8 : actualSegments;

        const double centerX = ellipse->center.x;
        const double centerY = ellipse->center.y;
        const double rx = ellipse->radiusX;
        float cr = ellipse->color[0], cg = ellipse->color[1], cb = ellipse->color[2];
        const double ry = ellipse->radiusY;
        const double rotation = ellipse->rotation;
        const double cosRot = std::cos(rotation);
        const double sinRot = std::sin(rotation);

        for (int i = 0; i <= actualSegments; ++i)
        {
            double t = static_cast<double>(i) / actualSegments;
            double angle = start + t * angleRange;
            double x = rx * std::cos(angle);
            double y = ry * std::sin(angle);

            outVertices.push_back({
                static_cast<float>(centerX + x * cosRot - y * sinRot),
                static_cast<float>(centerY + x * sinRot + y * cosRot),
                0.0f,
                cr, cg, cb
                });
        }
    }

    /**
     * @brief 结束场景模式（预留接口）
     *
     * @param dev 渲染设备指针
     */
    RENDER_API void renderEndScene(RenderDevice* dev)
    {
        if (!dev) return;
    }

    /**
     * @brief 发射多段线几何到渲染世界
     *
     * 将多段线细分为顶点并添加到渲染世界。
     *
     * @param dev 渲染设备指针
     * @param polyline 多段线几何数据
     */
    RENDER_API void renderEmitPolyline(RenderDevice* dev, const GeometryPolyline* polyline)
    {
        if (!dev || !polyline)
        {
            SY_ERROR("renderEmitPolyline: dev or polyline is null");
            return;
        }

        std::vector<render::VertexP3C3> vertices;
        tessellatePolyline(polyline, vertices);

        if (!vertices.empty())
        {
            render::PrimitiveType type = polyline->closed ?
                render::PrimitiveType::LineLoop : render::PrimitiveType::LineStrip;
            dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                static_cast<uint32_t>(vertices.size()), type, 0);
        }
        else
        {
            SY_WARN("renderEmitPolyline: no vertices generated");
        }
    }

    /**
     * @brief 发射圆形几何到渲染世界
     *
     * 将圆形细分为顶点并添加到渲染世界（使用LineLoop）。
     *
     * @param dev 渲染设备指针
     * @param circle 圆形几何数据
     */
    RENDER_API void renderEmitCircle(RenderDevice* dev, const GeometryCircle* circle)
    {
        if (!dev || !circle) return;

        std::vector<render::VertexP3C3> vertices;
        tessellateCircle(circle, vertices);

        if (!vertices.empty())
        {
            dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                static_cast<uint32_t>(vertices.size()),
                render::PrimitiveType::LineLoop, 0);
        }
    }

    /**
     * @brief 发射圆弧几何到渲染世界
     *
     * 将圆弧细分为顶点并添加到渲染世界（使用LineStrip）。
     *
     * @param dev 渲染设备指针
     * @param arc 圆弧几何数据
     */
    RENDER_API void renderEmitArc(RenderDevice* dev, const GeometryArc* arc)
    {
        if (!dev || !arc) return;

        std::vector<render::VertexP3C3> vertices;
        tessellateArc(arc, vertices);

        if (!vertices.empty())
        {
            dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                static_cast<uint32_t>(vertices.size()),
                render::PrimitiveType::LineStrip, 0);
        }
    }

    /**
     * @brief 发射椭圆几何到渲染世界
     *
     * 将椭圆细分为顶点并添加到渲染世界。
     *
     * @param dev 渲染设备指针
     * @param ellipse 椭圆几何数据
     */
    RENDER_API void renderEmitEllipse(RenderDevice* dev, const GeometryEllipse* ellipse)
    {
        if (!dev || !ellipse) return;

        std::vector<render::VertexP3C3> vertices;
        tessellateEllipse(ellipse, vertices);

        if (!vertices.empty())
        {
            render::PrimitiveType type = ellipse->fullEllipse ?
                render::PrimitiveType::LineLoop : render::PrimitiveType::LineStrip;
            dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                static_cast<uint32_t>(vertices.size()), type, 0);
        }
    }

    /**
     * @brief 发射文本几何到渲染世界（预留接口，尚未实现）
     *
     * @param dev 渲染设备指针
     * @param text 文本几何数据
     */
    RENDER_API void renderEmitText(RenderDevice* dev, const GeometryText* text)
    {
        if (!dev || !text || !text->text) return;

        RenderDevice::PendingText pt;
        pt.textStorage = text->text;
        pt.item.text = pt.textStorage.c_str();
        pt.item.x = static_cast<float>(text->position.x);
        pt.item.y = static_cast<float>(text->position.y);
        pt.item.coordMode = 0; // 世界坐标
        pt.item.hAlign = 0; // 左对齐
        pt.item.vAlign = 0; // 上对齐
        pt.item.fontSize = (text->fontSize > 0.0f) ? static_cast<int32_t>(text->fontSize) : 12;
        pt.item.color[0] = text->color[0];
        pt.item.color[1] = text->color[1];
        pt.item.color[2] = text->color[2];
        pt.item.color[3] = text->color[3];
        pt.item.rotationDeg = 0.0f;
        pt.item.zOrder = 0.0f;

        dev->pendingTextItems.push_back(std::move(pt));
    }

    /**
     * @brief 发射图像几何到渲染世界
     *
     * 将图像的四个角连接成线框（使用LineStrip）。
     *
     * @param dev 渲染设备指针
     * @param image 图像几何数据
     */
    RENDER_API void renderEmitImage(RenderDevice* dev, const GeometryImage* image)
    {
        if (!dev || !image) return;

        std::vector<render::VertexP3C3> vertices;
        vertices.reserve(5);

        render::VertexP3C3 v;
        v.cr = image->color[0]; v.cg = image->color[1]; v.cb = image->color[2];

        v.px = static_cast<float>(image->topLeft.x); v.py = static_cast<float>(image->topLeft.y); v.pz = 0.0f;
        vertices.push_back(v);
        v.px = static_cast<float>(image->topRight.x); v.py = static_cast<float>(image->topRight.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->bottomRight.x); v.py = static_cast<float>(image->bottomRight.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->bottomLeft.x); v.py = static_cast<float>(image->bottomLeft.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->topLeft.x); v.py = static_cast<float>(image->topLeft.y);
        vertices.push_back(v);

        dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
            static_cast<uint32_t>(vertices.size()),
            render::PrimitiveType::LineStrip, 0);
    }

    RENDER_API void renderEmitTriangleSoup(RenderDevice* dev,
        const float* vertices, const float* normals,
        uint32_t vertexCount,
        const float color[4])
    {
        if (!dev || !vertices || !normals || vertexCount < 3) return;

        // 生成顺序索引 (0, 1, 2, 3, 4, 5, ...)
        std::vector<uint32_t> indices(vertexCount);
        for (uint32_t i = 0; i < vertexCount; ++i) indices[i] = i;

        MeshId meshId = dev->meshManager.registerMesh(vertices, normals,
            indices.data(), vertexCount,
            vertexCount);
        if (meshId == INVALID_MESH_ID) return;

        // 默认单位矩阵作为模型变换
        float identity[16] = {
            1,0,0,0,
            0,1,0,0,
            0,0,1,0,
            0,0,0,1
        };
        float col[4];
        if (color)
        {
            col[0] = color[0]; col[1] = color[1];
            col[2] = color[2]; col[3] = color[3];
        }
        else
        {
            col[0] = col[1] = col[2] = 0.7f; col[3] = 1.0f;
        }
        dev->meshManager.addInstance(meshId, identity, 0, col);
    }
}