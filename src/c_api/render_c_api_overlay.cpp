/**
 * @file render_c_api_overlay.cpp
 * @brief 统一 Overlay API、场景环境
 *
 * 从 render_c_api.cpp 拆分而来，包含：
 * - 统一 Overlay API（renderSubmitOverlay / renderClearOverlays）
 * - 场景环境（网格背景等）
 * - 文本渲染（renderSetTexts）
 * - 位图接口（预留）
 *
 * 旧版 Overlay 兼容包装器（renderSetOverlay / renderSetPreviewLines /
 * renderSetControlLines / renderSetPointMarkers / renderSetSelectionBox /
 * renderSetSelectionRect / renderSetSelectionHandles）已移除，
 * 所有调用方已迁移到统一 API。
 */
#include "render_c_api_internal.h"

using namespace render;

extern "C" {
    // ==================== 文本渲染 ====================

    /**
     * @brief 设置要渲染的文本列表
     *
     * @param dev 渲染设备指针
     * @param texts 文本项列表
     */
    RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts)
    {
        if (!dev) return;
        dev->textAtlas.renderText(texts, dev->view2D.viewMatrix,
            static_cast<uint32_t>(dev->view2D.viewWidth),
            static_cast<uint32_t>(dev->view2D.viewHeight),
            dev->rhiDevice);
    }

    // ==================== Overlay 统一 API ====================

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
     * @brief 按生命周期分组清除通过统一 API 提交的叠加层图元
     */
    RENDER_API void renderClearOverlayGroup(RenderDevice* dev, OverlayGroup group)
    {
        if (!dev) return;
        dev->overlayQueue.clearOverlayGroup(group);
    }

    // ==================== 场景环境 ====================

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
} // extern "C"
