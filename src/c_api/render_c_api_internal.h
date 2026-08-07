/**
 * @file render_c_api_internal.h
 * @brief C API 内部共享头文件
 *
 * 提供 RenderDevice 结构体定义和公共 include，供所有拆分后的子文件共享。
 * 不对外暴露，仅用于 Renderx 内部编译。
 *
 * @note RenderDevice 当前是一个聚合体，包含会话级状态（view、overlay、text、selection、stats）、
 *       场景级资源（world2D、meshManager、environment）和后端级资源（rhiDevice、shader cache、pipeline cache）。
 *       后续重构应将这些状态分离到 RenderRuntime（共享资源）和 RenderSession（会话私有状态）。
 */
#pragma once

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
#include "core/screen_text_renderer.h"
#include "core/scene_env.h"
#include "rhi/rhi_device.h"
#include "rhi/rhi_gl.h"
#include "rhi/rhi_null.h"
#include "shader/shaders.h"
#include "render_runtime.h"

#include <cstring>
#include <string>
#include <vector>
#include <cassert>
#include <cmath>
#include <cfloat>
#include <filesystem>
#include <fstream>

#include "Log/SyLogger.h"

namespace render
{
    // 前向声明 RenderRuntime
    struct RenderRuntime;

    /**
     * @brief 渲染设备内部结构（会话级容器）
     *
     * 包含所有渲染模块的实例和状态信息。
     *
     * @note M1/M2 改动：RenderDevice 逐步向 RenderSession 演进。
     *       共享级资源（如 shader 缓存）已迁移到 RenderRuntime。
     *       会话级状态（view、overlay、text、状态）保留在 RenderDevice 中。
     *       后续将通过 RenderDevice* -> RenderSession* 的 typedef 渐进迁名。
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
        /// 屏幕文本渲染器
        core::ScreenTextRenderer screenTextRenderer;
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

        /// 暂存的屏幕文本项（在 renderFrame 中渲染）
        struct PendingScreenText
        {
            std::string text;
            float x, y;
            float color[4];
            float fontSize;
        };
        std::vector<PendingScreenText> pendingScreenTexts;

        /// 视口像素尺寸（由 renderResize 更新，供 TextAtlas 等模块使用）
        uint32_t viewportWidth = 0;
        uint32_t viewportHeight = 0;

        /// 图元 ID 自动计数器（实例级，避免多设备间 ID 冲突）
        uint64_t entityIdCounter = 1;

        /// 相机中心世界坐标（double 精度，用于 camera-relative 渲染消除大坐标浮点精度问题）
        double cameraCenter[2] = { 0.0, 0.0 };
    };

    /// M2: RenderSession = RenderDevice 的别名
    /// 当前阶段保持别名以维持 C API 兼容性
    /// 未来: RenderDevice 会被重命名为 RenderSession，RenderRuntime 管理共享资源
    using RenderSession = RenderDevice;
}