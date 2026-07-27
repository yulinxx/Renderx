/**
 * @file render_c_api.cpp
 * @brief C API 实现文件
 *
 * 提供 Renderx 渲染框架的 C 语言 API，作为外部应用程序与渲染核心之间的桥梁。
 * 实现了设备创建/销毁、图元管理、视图设置、渲染帧等核心功能。
 */
#include "render/render.h"
#include "render/render_types.h"
#include "core/render_world.h"
#include "core/batch_queue.h"
#include "core/overlay_queue.h"
#include "core/command_encoder.h"
#include "core/render_graph.h"
#include "core/pipeline_state_manager.h"
#include "core/draw_batcher.h"
#include "core/persistent_entity_manager.h"
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

        /// 2D 渲染世界（图元管理和可见性查询）
        core::RenderWorld    world2D;
        /// 批处理队列（2D 图元批处理渲染）
        core::BatchQueue     batchQueue;
        /// 叠加层队列（UI 元素渲染）
        core::OverlayQueue   overlayQueue;
        /// 网格管理器（3D 网格实例化渲染）
        core::MeshManager    meshManager;
        /// 文本贴图管理器（文本渲染）
        core::TextAtlas      textAtlas;
        /// 场景环境渲染器（网格背景等）
        core::SceneEnv       sceneEnv;
        /// 统一命令编码器（Phase 3 新增，统一 overlay / world 绘制命令）
        core::CommandEncoder commandEncoder;
        /// 显式 Pass 调度器（Phase 4 新增，线性顺序执行器）
        core::RenderGraph    renderGraph;
        /// 管线状态管理器（Phase 7 新增，缓存与复用 RHI 管线）
        core::PipelineStateManager pipelineStateManager;
        /// 绘制合批器（Phase 8 新增，overlay MDI 合批）
        core::DrawBatcher    drawBatcher;
        /// 持久图元管理器（Phase 9 新增，SSBO + GPU 剔除）
        core::PersistentEntityManager persistentEntityManager;

        /// 视图模式（2D/3D）
        ViewMode             viewMode = ViewMode::Mode2D;

        /// 清屏颜色（由 renderSetClearColor 设置）
        float                clearColor[4] = { 0.94f, 0.94f, 0.94f, 1.0f };

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

        /// 可见图元索引缓存
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
        dev->commandEncoder.initialize(dev->rhiDevice);
        dev->renderGraph.initialize(dev->rhiDevice);
        dev->pipelineStateManager.initialize(dev->rhiDevice);
        // Phase 7: 将 PSM 注入 CommandEncoder，使其复用管线缓存
        dev->commandEncoder.setPipelineStateManager(&dev->pipelineStateManager);
        // Phase 8: 初始化 DrawBatcher 并注入 CommandEncoder
        dev->drawBatcher.initialize(dev->rhiDevice);
        dev->commandEncoder.setDrawBatcher(&dev->drawBatcher);
        // Phase 9: 初始化持久图元管理器（默认容量 65536）
        dev->persistentEntityManager.initialize(dev->rhiDevice, 65536);
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
        dev->persistentEntityManager.shutdown(); // Phase 9
        dev->drawBatcher.shutdown();       // Phase 8
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
        if (!dev || !dev->rhiDevice) return;
        dev->rhiDevice->resize(width, height);
    }

    /**
     * @brief 添加图元到渲染世界
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
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
     * @brief 修改图元的顶点数据
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
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
     * @brief 从渲染世界移除图元
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     */
    RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id)
    {
        if (!dev) return;
        dev->world2D.removeEntity(id);
    }

    /**
     * @brief 设置图元可见性
     *
     * @param dev 渲染设备指针
     * @param id 图元ID
     * @param visible 是否可见（非0表示可见）
     */
    RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible)
    {
        if (!dev) return;
        dev->world2D.setEntityVisibility(id, visible != 0);
    }

    /**
     * @brief 批量应用图元更新
     *
     * 从更新数据包中解析并应用一系列图元操作（添加、修改、删除）。
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

    RENDER_API void renderSetClearColor(RenderDevice* dev, float r, float g, float b, float a)
    {
        if (!dev) return;
        dev->clearColor[0] = r;
        dev->clearColor[1] = g;
        dev->clearColor[2] = b;
        dev->clearColor[3] = a;
        if (dev->rhiDevice)
        {
            dev->rhiDevice->setClearColor(r, g, b, a);
        }
    }

    /**
     * @brief 设置叠加层数据
     *
     * Phase 1 起，十字准星和捕捉指示器通过统一 API 提交。
     *
     * @param dev 渲染设备指针
     * @param overlay 叠加层数据
     */
    RENDER_API void renderSetOverlay(RenderDevice* dev, const OverlayData* overlay)
    {
        if (!dev || !overlay) return;
        dev->overlay = *overlay;

        // 十字准星通过统一 API 提交
        if (overlay->crosshairVisible != 0)
        {
            float pos[2] = { overlay->crosshairWorld[0], overlay->crosshairWorld[1] };
            OverlayMarkerSetDesc desc;
            desc.positions = pos;
            desc.count = 1;

            OverlayPrimitive prim;
            prim.kind = OverlayPrimitiveKind::Crosshair;
            prim.flags = 0;
            prim.payload = &desc;
            prim.payloadSize = sizeof(desc);
            prim.style.borderColor = 0xFFFFFFFF; // 白色
            prim.style.pointSize = 20.0f; // 十字线半长
            dev->overlayQueue.submitOverlay(&prim);
        }

        // 捕捉指示器通过统一 API 提交
        if (overlay->snapVisible != 0)
        {
            float pos[2] = { overlay->snapWorld[0], overlay->snapWorld[1] };
            OverlayMarkerSetDesc desc;
            desc.positions = pos;
            desc.count = 1;

            // 将 float[4] 颜色转换为 RGBA32
            uint32_t rgba = 0;
            auto f2u = [](float f) -> uint32_t {
                int v = static_cast<int>(f * 255.0f + 0.5f);
                return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            };
            rgba |= f2u(overlay->snapColor[0]) << 0;
            rgba |= f2u(overlay->snapColor[1]) << 8;
            rgba |= f2u(overlay->snapColor[2]) << 16;
            rgba |= f2u(overlay->snapColor[3]) << 24;

            OverlayPrimitive prim;
            prim.kind = OverlayPrimitiveKind::SnapIndicator;
            prim.flags = 0;
            prim.payload = &desc;
            prim.payloadSize = sizeof(desc);
            prim.style.fillColor = rgba;
            prim.style.pointSize = 8.0f; // 圆半径
            dev->overlayQueue.submitOverlay(&prim);
        }
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
     * Phase 1 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitOverlay 提交。
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetPreviewLines(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, uint32_t colorRGBA)
    {
        if (!dev || !vertices || vertexCount == 0) return;

        // 提取位置数组（每顶点3个float）
        std::vector<float> positions;
        positions.resize(static_cast<size_t>(vertexCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            positions[i * 3 + 0] = vertices[i].px;
            positions[i * 3 + 1] = vertices[i].py;
            positions[i * 3 + 2] = vertices[i].pz;
        }

        // 提取逐顶点颜色数组（每顶点3个float）
        std::vector<float> colors;
        colors.resize(static_cast<size_t>(vertexCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            colors[i * 3 + 0] = vertices[i].cr;
            colors[i * 3 + 1] = vertices[i].cg;
            colors[i * 3 + 2] = vertices[i].cb;
        }

        OverlayPolylineDesc desc;
        desc.vertices = positions.data();
        desc.vertexCount = vertexCount;
        desc.usePerVertexColor = true;
        desc.colors = colors.data();

        OverlayPrimitive prim;
        prim.kind = OverlayPrimitiveKind::LineList;
        prim.flags = 0;
        prim.payload = &desc;
        prim.payloadSize = sizeof(desc);
        prim.style.borderColor = colorRGBA;
        prim.style.lineWidth = 1.0f;

        dev->overlayQueue.submitOverlay(&prim);
    }

    /**
     * @brief 设置控制线
     *
     * Phase 1 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitOverlay 提交。
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点数据
     * @param vertexCount 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetControlLines(RenderDevice* dev, const VertexP3C3* vertices,
        uint32_t vertexCount, uint32_t colorRGBA)
    {
        if (!dev || !vertices || vertexCount == 0) return;

        std::vector<float> positions;
        positions.resize(static_cast<size_t>(vertexCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            positions[i * 3 + 0] = vertices[i].px;
            positions[i * 3 + 1] = vertices[i].py;
            positions[i * 3 + 2] = vertices[i].pz;
        }

        std::vector<float> colors;
        colors.resize(static_cast<size_t>(vertexCount) * 3);
        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            colors[i * 3 + 0] = vertices[i].cr;
            colors[i * 3 + 1] = vertices[i].cg;
            colors[i * 3 + 2] = vertices[i].cb;
        }

        OverlayPolylineDesc desc;
        desc.vertices = positions.data();
        desc.vertexCount = vertexCount;
        desc.usePerVertexColor = true;
        desc.colors = colors.data();

        OverlayPrimitive prim;
        prim.kind = OverlayPrimitiveKind::LineList;
        prim.flags = 0;
        prim.payload = &desc;
        prim.payloadSize = sizeof(desc);
        prim.style.borderColor = colorRGBA;
        prim.style.lineWidth = 1.0f;

        dev->overlayQueue.submitOverlay(&prim);
    }

    /**
     * @brief 设置点标记
     *
     * Phase 6 起此函数改为统一 API 的兼容包装器。
     * 有数据时通过 renderSubmitOverlay 提交，无数据时通过 clearOverlayKind 清除。
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

        dev->overlayQueue.clearOverlayKind(OverlayPrimitiveKind::Points);
        if (!worldPositions || count == 0)
            return;

        OverlayMarkerSetDesc desc;
        desc.positions = worldPositions;
        desc.count = count;

        OverlayPrimitive prim;
        prim.kind = OverlayPrimitiveKind::Points;
        prim.flags = 0;
        prim.payload = &desc;
        prim.payloadSize = sizeof(desc);
        prim.style.fillColor = fillColor;
        prim.style.borderColor = borderColor;
        prim.style.pointSize = markerSize;

        dev->overlayQueue.submitOverlay(&prim);
    }

    /**
     * @brief 设置选择框
     *
     * Phase 6 起此函数改为统一 API 的兼容包装器。
     * 有数据时通过 renderSubmitOverlay 提交，无数据时通过 clearOverlayKind 清除。
     *
     * @param dev 渲染设备指针
     * @param bbox 边界框
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    RENDER_API void renderSetSelectionBox(RenderDevice* dev, const BBox2f* bbox, uint32_t colorRGBA)
    {
        if (!dev) return;

        dev->overlayQueue.clearOverlayKind(OverlayPrimitiveKind::Rect);
        if (!bbox)
            return;

        OverlayRectDesc desc;
        desc.minX = bbox->minX; desc.minY = bbox->minY;
        desc.maxX = bbox->maxX; desc.maxY = bbox->maxY;

        OverlayPrimitive prim;
        prim.kind = OverlayPrimitiveKind::Rect;
        prim.flags = 0;
        prim.payload = &desc;
        prim.payloadSize = sizeof(desc);
        prim.style.borderColor = colorRGBA;
        prim.style.lineWidth = 1.0f;

        dev->overlayQueue.submitOverlay(&prim);
    }

    /**
     * @brief 设置选择预览矩形
     *
     * Phase 6 起此函数改为统一 API 的兼容包装器。
     * 若填充色 alpha 不为 0，会提交两个图元：FilledRect（填充）+ Rect（边框）。
     *
     * @param dev 渲染设备指针
     * @param bbox 边界框
     * @param fillColor 填充颜色（32位RGBA格式，alpha=0时无填充）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    RENDER_API void renderSetSelectionRect(RenderDevice* dev, const BBox2f* bbox,
        uint32_t fillColor, uint32_t borderColor)
    {
        if (!dev) return;

        dev->overlayQueue.clearOverlayKind(OverlayPrimitiveKind::FilledRect);
        dev->overlayQueue.clearOverlayKind(OverlayPrimitiveKind::Rect);
        if (!bbox)
            return;

        OverlayRectDesc desc;
        desc.minX = bbox->minX; desc.minY = bbox->minY;
        desc.maxX = bbox->maxX; desc.maxY = bbox->maxY;

        uint8_t fillAlpha = static_cast<uint8_t>((fillColor >> 24) & 0xFF);
        if (fillAlpha != 0)
        {
            OverlayPrimitive fillPrim;
            fillPrim.kind = OverlayPrimitiveKind::FilledRect;
            fillPrim.flags = 0;
            fillPrim.payload = &desc;
            fillPrim.payloadSize = sizeof(desc);
            fillPrim.style.fillColor = fillColor;
            dev->overlayQueue.submitOverlay(&fillPrim);
        }

        OverlayPrimitive borderPrim;
        borderPrim.kind = OverlayPrimitiveKind::Rect;
        borderPrim.flags = 0;
        borderPrim.payload = &desc;
        borderPrim.payloadSize = sizeof(desc);
        borderPrim.style.borderColor = borderColor;
        borderPrim.style.lineWidth = 1.0f;
        dev->overlayQueue.submitOverlay(&borderPrim);
    }

    /**
     * @brief 设置选择手柄
     *
     * Phase 6 起此函数改为统一 API 的兼容包装器。
     * 有数据时通过 renderSubmitOverlay 提交，无数据时通过 clearOverlayKind 清除。
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

        dev->overlayQueue.clearOverlayKind(OverlayPrimitiveKind::Points);
        if (!worldPositions || count == 0)
            return;

        OverlayMarkerSetDesc desc;
        desc.positions = worldPositions;
        desc.count = count;

        OverlayPrimitive prim;
        prim.kind = OverlayPrimitiveKind::Points;
        prim.flags = 0;
        prim.payload = &desc;
        prim.payloadSize = sizeof(desc);
        prim.style.fillColor = fillColor;
        prim.style.borderColor = borderColor;
        prim.style.pointSize = handleSize;

        dev->overlayQueue.submitOverlay(&prim);
    }

    /**
     * @brief 提交单个叠加层图元（统一 API）
     */
    RENDER_API void renderSubmitOverlay(RenderDevice* dev, const OverlayPrimitive* primitive)
    {
        if (!dev || !primitive) return;
        dev->overlayQueue.submitOverlay(primitive);
    }

    /**
     * @brief 批量提交叠加层图元（统一 API）
     */
    RENDER_API void renderSubmitOverlays(RenderDevice* dev, const OverlayPrimitive* primitives, uint32_t count)
    {
        if (!dev || !primitives || count == 0) return;
        for (uint32_t i = 0; i < count; ++i)
            dev->overlayQueue.submitOverlay(&primitives[i]);
    }

    /**
     * @brief 清除所有通过统一 API 提交的叠加层图元
     */
    RENDER_API void renderClearOverlays(RenderDevice* dev)
    {
        if (!dev) return;
        dev->overlayQueue.clearUnifiedOverlays();
    }

    /**
     * @brief 按类型清除通过统一 API 提交的叠加层图元
     */
    RENDER_API void renderClearOverlayKind(RenderDevice* dev, OverlayPrimitiveKind kind)
    {
        if (!dev) return;
        dev->overlayQueue.clearOverlayKind(kind);
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
        const float* layerWidths, const bool* pixelFlags,
        const bool* triangleFlags, const float* zDepths)
    {
        if (!dev) return;
        dev->sceneEnv.setGeometryEx(vertices, vertexCount, layerOffsets, layerCount,
            layerColors, layerWidths, pixelFlags, triangleFlags, zDepths);
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
     * 3. 查询可见图元
     * 4. 渲染场景环境（网格背景）
     * 5. 渲染2D图元批处理
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

        uint32_t visibleCount = 0;

        // Phase 4: 清空上一帧的 Pass，根据当前视图模式重新编排（线性顺序执行）
        dev->renderGraph.clear();

        if (dev->viewMode == ViewMode::Mode2D)
        {
            // ---- CPU 侧数据准备（不涉及 RHI，放在 Pass 外）----
            dev->world2D.update();

            uint32_t maxVisible = static_cast<uint32_t>(dev->world2D.getEntityCount());
            if (dev->visibleIndices.size() < maxVisible)
                dev->visibleIndices.resize(maxVisible);

            dev->world2D.queryVisible(dev->view2D.viewMatrix, dev->view2D.viewWidth,
                dev->view2D.viewHeight, dev->visibleIndices.data(), &visibleCount, maxVisible);

            if (maxVisible > 0 && visibleCount == 0)
            {
                SY_DEBUGF("renderFrame: queryVisible returned 0 (total=%u), forcing all entities for debug", maxVisible);
                visibleCount = maxVisible;
                for (uint32_t i = 0; i < maxVisible; ++i)
                    dev->visibleIndices[i] = i;
            }

            SY_INFOF("R2D:e=%u v=%u s=%.4f tx=%.2f ty=%.2f vp=%.0fx%.0f",
                maxVisible, visibleCount,
                dev->view2D.viewMatrix[0],
                dev->view2D.viewMatrix[6],
                dev->view2D.viewMatrix[7],
                dev->view2D.viewWidth, dev->view2D.viewHeight);

            // 预先把 world2D 的可见图元提交给 BatchQueue（生成间接命令）
            dev->batchQueue.submit(dev->visibleIndices.data(), visibleCount, dev->world2D);

            // ---- Pass 0: FrameSetup ----
            // 设置清屏颜色、深度测试、混合状态，并重置命令编码器
            {
                core::PassDesc pass;
                pass.name = "FrameSetup";
                pass.enabled = true;
                pass.onSetup = [dev](rhi::IDevice* d) {
                    d->setClearColor(dev->clearColor[0], dev->clearColor[1],
                        dev->clearColor[2], dev->clearColor[3]);
                    d->enableDepthTest(false);
                    d->enableBlend(true);
                    dev->commandEncoder.reset();
                };
                // Phase 5: 资源依赖描述（为后续自动屏障管理预留）
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 1: SceneEnv ----
            // 渲染场景环境（网格背景等）
            {
                core::PassDesc pass;
                pass.name = "SceneEnv";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->sceneEnv.render(d, dev->view2D.viewMatrix,
                        static_cast<uint32_t>(dev->view2D.viewWidth),
                        static_cast<uint32_t>(dev->view2D.viewHeight));
                };
                pass.inputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Read, "Backbuffer", 0 });
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 2: World2DCollect ----
            // 将 world2D 图元渲染命令收集到 CommandEncoder
            {
                core::PassDesc pass;
                pass.name = "World2DCollect";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->batchQueue.render(d, &dev->commandEncoder,
                        dev->view2D.viewMatrix, dev->world2D);
                };
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "World2D_VB", 0 });
                pass.inputs.push_back({ core::PassResourceType::IndirectBuffer,
                    core::PassResourceAccess::Read, "BatchQueue_IB", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 3: OverlayCollect ----
            // 将 overlay 渲染命令收集到 CommandEncoder
            {
                core::PassDesc pass;
                pass.name = "OverlayCollect";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->overlayQueue.render(d, &dev->commandEncoder, dev->view2D.viewMatrix);
                };
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "OverlayQueue_VB", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 4: CommandExecute ----
            // 统一执行所有已收集的绘制命令（World2D + Overlay）
            {
                core::PassDesc pass;
                pass.name = "CommandExecute";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->commandEncoder.execute(d,
                        dev->batchQueue.getVertexBuffer(),
                        dev->overlayQueue.getVertexBuffer(),
                        dev->batchQueue.getIndirectBuffer(),
                        dev->view2D.viewMatrix);
                };
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "BatchQueue_VB", 0 });
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "OverlayQueue_VB", 0 });
                pass.inputs.push_back({ core::PassResourceType::IndirectBuffer,
                    core::PassResourceAccess::Read, "BatchQueue_IB", 0 });
                pass.inputs.push_back({ core::PassResourceType::UniformBuffer,
                    core::PassResourceAccess::Read, "ViewMatrix_UB", 0 });
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 5: Text ----
            // 渲染文本（在 overlay 之上）
            if (!dev->pendingTextItems.empty())
            {
                core::PassDesc pass;
                pass.name = "Text";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    if (dev->pendingTextItems.empty()) return;
                    std::vector<TextItem> items;
                    items.reserve(dev->pendingTextItems.size());
                    for (auto& pt : dev->pendingTextItems)
                        items.push_back(pt.item);
                    TextItemList list;
                    list.items = items.data();
                    list.count = static_cast<uint32_t>(items.size());
                    dev->textAtlas.renderText(&list, dev->view2D.viewMatrix, d);
                    dev->pendingTextItems.clear();
                };
                pass.inputs.push_back({ core::PassResourceType::Texture,
                    core::PassResourceAccess::Read, "TextAtlas_Tex", 0 });
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "Text_VB", 0 });
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }
        }
        else
        {
            // ---- 3D 模式 Pass 编排 ----

            // Pass 0: FrameSetup3D
            {
                core::PassDesc pass;
                pass.name = "FrameSetup3D";
                pass.enabled = true;
                pass.onSetup = [](rhi::IDevice* d) {
                    d->setClearColor(0.12f, 0.14f, 0.20f, 1.0f);
                    d->enableDepthTest(true);
                    d->enableBlend(true);
                };
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                pass.outputs.push_back({ core::PassResourceType::DepthTarget,
                    core::PassResourceAccess::Write, "DepthBuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // Pass 1: Mesh3D
            if (dev->meshManager.getInstanceCount() > 0)
            {
                core::PassDesc pass;
                pass.name = "Mesh3D";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    if (dev->meshManager.getInstanceCount() == 0) return;
                    dev->meshManager.update();
                    dev->meshManager.render(d, dev->view3D.viewMatrix, dev->view3D.projMatrix);
                };
                pass.inputs.push_back({ core::PassResourceType::VertexBuffer,
                    core::PassResourceAccess::Read, "MeshManager_VB", 0 });
                pass.inputs.push_back({ core::PassResourceType::IndexBuffer,
                    core::PassResourceAccess::Read, "MeshManager_IB", 0 });
                pass.inputs.push_back({ core::PassResourceType::UniformBuffer,
                    core::PassResourceAccess::Read, "ViewProj_UB", 0 });
                pass.outputs.push_back({ core::PassResourceType::ColorTarget,
                    core::PassResourceAccess::Write, "Backbuffer", 0 });
                pass.outputs.push_back({ core::PassResourceType::DepthTarget,
                    core::PassResourceAccess::Write, "DepthBuffer", 0 });
                dev->renderGraph.addPass(pass);
            }
        }

        // Phase 4: 按 Pass 顺序统一执行
        dev->renderGraph.execute(rhi);

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
     * @brief 获取图元数量
     *
     * @param dev 渲染设备指针
     * @return 图元数量
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

    /// 场景模式下的图元ID计数器
    static uint64_t s_entityIdCounter = 1;

    /**
     * @brief 开始场景模式
     *
     * 清除所有旧图元，重置图元ID计数器，准备接收新的场景数据。
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
        SY_INFOF("renderBeginScene: clearing world2D (had %u entities)", dev->world2D.getEntityCount());
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
        SY_INFOF("renderEndScene: world2D now has %u entities", dev->world2D.getEntityCount());
    }

    /**
     * @brief 发射多段线几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
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

        static uint32_t s_emitCount = 0;
        if (s_emitCount < 3)
        {
            SY_INFOF("renderEmitPolyline[%u]: points=%u, closed=%d, firstPt=(%.2f,%.2f), color=(%.2f,%.2f,%.2f)",
                s_emitCount, polyline->pointCount, polyline->closed ? 1 : 0,
                polyline->points[0].x, polyline->points[0].y,
                polyline->color[0], polyline->color[1], polyline->color[2]);
            s_emitCount++;
        }

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Polyline;
        prim.flags = 0;
        prim.desc.polyline = polyline;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射圆形几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param circle 圆形几何数据
     */
    RENDER_API void renderEmitCircle(RenderDevice* dev, const GeometryCircle* circle)
    {
        if (!dev || !circle) return;

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Circle;
        prim.flags = 0;
        prim.desc.circle = circle;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射圆弧几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param arc 圆弧几何数据
     */
    RENDER_API void renderEmitArc(RenderDevice* dev, const GeometryArc* arc)
    {
        if (!dev || !arc) return;

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Arc;
        prim.flags = 0;
        prim.desc.arc = arc;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射椭圆几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param ellipse 椭圆几何数据
     */
    RENDER_API void renderEmitEllipse(RenderDevice* dev, const GeometryEllipse* ellipse)
    {
        if (!dev || !ellipse) return;

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Ellipse;
        prim.flags = 0;
        prim.desc.ellipse = ellipse;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射文本几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param text 文本几何数据
     */
    RENDER_API void renderEmitText(RenderDevice* dev, const GeometryText* text)
    {
        if (!dev || !text || !text->text) return;

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Text;
        prim.flags = 0;
        prim.desc.text = text;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射图像几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param image 图像几何数据
     */
    RENDER_API void renderEmitImage(RenderDevice* dev, const GeometryImage* image)
    {
        if (!dev || !image) return;

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::Image;
        prim.flags = 0;
        prim.desc.image = image;
        renderSubmitGeometry(dev, &prim);
    }

    /**
     * @brief 发射三角网格几何到渲染世界
     *
     * Phase 2 起此函数变为统一 API 的兼容包装器，内部通过 renderSubmitGeometry 提交。
     *
     * @param dev 渲染设备指针
     * @param vertices 顶点位置数组
     * @param normals 顶点法线数组
     * @param vertexCount 顶点数量
     * @param color RGBA颜色，为null时使用默认灰色
     */
    RENDER_API void renderEmitTriangleSoup(RenderDevice* dev,
        const float* vertices, const float* normals,
        uint32_t vertexCount,
        const float color[4])
    {
        if (!dev || !vertices || !normals || vertexCount < 3) return;

        // 组装 typed desc 并转发到统一入口
        GeometryTriangleSoupDesc desc;
        desc.vertices = vertices;
        desc.normals = normals;
        desc.vertexCount = vertexCount;
        if (color)
        {
            desc.color[0] = color[0];
            desc.color[1] = color[1];
            desc.color[2] = color[2];
            desc.color[3] = color[3];
        }
        else
        {
            desc.color[0] = desc.color[1] = desc.color[2] = 0.7f;
            desc.color[3] = 1.0f;
        }

        GeometryPrimitive prim;
        prim.kind = GeometryPrimitiveKind::TriangleSoup;
        prim.flags = 0;
        prim.desc.triangleSoup = &desc;
        renderSubmitGeometry(dev, &prim);
    }

    // ========================================================================
    // Phase 2: 统一几何提交 API 实现
    // ========================================================================

    /**
     * @brief 提交单个几何图元（统一 API）
     *
     * 根据 GeometryPrimitive::kind 分发到对应渲染路径：
     * - Polyline / Circle / Arc / Ellipse / Image → tessellate 后 world2D.addEntity
     * - Text → 暂存到 pendingTextItems，renderFrame 时由 TextAtlas 渲染
     * - TriangleSoup → meshManager.registerMesh + addInstance
     */
    RENDER_API void renderSubmitGeometry(RenderDevice* dev, const GeometryPrimitive* primitive)
    {
        if (!dev || !primitive) return;

        switch (primitive->kind)
        {
            // ---- 2D 文档几何路径 ----
            case GeometryPrimitiveKind::Polyline:
            {
                if (!primitive->desc.polyline) return;
                std::vector<render::VertexP3C3> vertices;
                tessellatePolyline(primitive->desc.polyline, vertices);
                if (!vertices.empty())
                {
                    render::PrimitiveType type = primitive->desc.polyline->closed ?
                        render::PrimitiveType::LineLoop : render::PrimitiveType::LineStrip;
                    dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                        static_cast<uint32_t>(vertices.size()), type, 0);
                }
                break;
            }
            case GeometryPrimitiveKind::Circle:
            {
                if (!primitive->desc.circle) return;
                std::vector<render::VertexP3C3> vertices;
                tessellateCircle(primitive->desc.circle, vertices);
                if (!vertices.empty())
                {
                    dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                        static_cast<uint32_t>(vertices.size()),
                        render::PrimitiveType::LineLoop, 0);
                }
                break;
            }
            case GeometryPrimitiveKind::Arc:
            {
                if (!primitive->desc.arc) return;
                std::vector<render::VertexP3C3> vertices;
                tessellateArc(primitive->desc.arc, vertices);
                if (!vertices.empty())
                {
                    dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                        static_cast<uint32_t>(vertices.size()),
                        render::PrimitiveType::LineStrip, 0);
                }
                break;
            }
            case GeometryPrimitiveKind::Ellipse:
            {
                if (!primitive->desc.ellipse) return;
                std::vector<render::VertexP3C3> vertices;
                tessellateEllipse(primitive->desc.ellipse, vertices);
                if (!vertices.empty())
                {
                    render::PrimitiveType type = primitive->desc.ellipse->fullEllipse ?
                        render::PrimitiveType::LineLoop : render::PrimitiveType::LineStrip;
                    dev->world2D.addEntity(s_entityIdCounter++, vertices.data(),
                        static_cast<uint32_t>(vertices.size()), type, 0);
                }
                break;
            }
            case GeometryPrimitiveKind::Image:
            {
                if (!primitive->desc.image) return;
                const GeometryImage* image = primitive->desc.image;
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
                break;
            }
            // ---- 文本缓存路径 ----
            case GeometryPrimitiveKind::Text:
            {
                if (!primitive->desc.text || !primitive->desc.text->text) return;
                const GeometryText* text = primitive->desc.text;
                RenderDevice::PendingText pt;
                pt.textStorage = text->text;
                pt.item.text = pt.textStorage.c_str();
                pt.item.x = static_cast<float>(text->position.x);
                pt.item.y = static_cast<float>(text->position.y);
                pt.item.coordMode = 0;
                pt.item.hAlign = 0;
                pt.item.vAlign = 0;
                pt.item.fontSize = (text->fontSize > 0.0f) ? static_cast<int32_t>(text->fontSize) : 12;
                pt.item.color[0] = text->color[0];
                pt.item.color[1] = text->color[1];
                pt.item.color[2] = text->color[2];
                pt.item.color[3] = text->color[3];
                pt.item.rotationDeg = 0.0f;
                pt.item.zOrder = 0.0f;
                dev->pendingTextItems.push_back(std::move(pt));
                break;
            }
            // ---- 3D mesh 路径 ----
            case GeometryPrimitiveKind::TriangleSoup:
            {
                if (!primitive->desc.triangleSoup) return;
                const GeometryTriangleSoupDesc* desc = primitive->desc.triangleSoup;
                if (!desc->vertices || !desc->normals || desc->vertexCount < 3) return;

                // 生成顺序索引
                std::vector<uint32_t> indices(desc->vertexCount);
                for (uint32_t i = 0; i < desc->vertexCount; ++i) indices[i] = i;

                MeshId meshId = dev->meshManager.registerMesh(desc->vertices, desc->normals,
                    indices.data(), desc->vertexCount, desc->vertexCount);
                if (meshId == INVALID_MESH_ID) return;

                // 默认单位矩阵作为模型变换
                float identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
                dev->meshManager.addInstance(meshId, identity, 0, const_cast<float*>(desc->color));
                break;
            }
        }
    }

    /**
     * @brief 批量提交几何图元（统一 API）
     */
    RENDER_API void renderSubmitGeometries(RenderDevice* dev, const GeometryPrimitive* primitives, uint32_t count)
    {
        if (!dev || !primitives || count == 0) return;
        for (uint32_t i = 0; i < count; ++i)
            renderSubmitGeometry(dev, &primitives[i]);
    }
}