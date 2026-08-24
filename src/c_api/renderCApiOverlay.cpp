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
#include "renderCApiInternal.h"

using namespace Render;

extern "C"
{
    // ==================== 文本渲染 ====================

    RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts)
    {
        if (!dev)
        {
            return;
        }
        dev->renderTexts(texts);
    }

    // ==================== Overlay 统一 API ====================

    RENDER_API void renderSubmitOverlay(RenderDevice* dev, const OverlayPrimitive* primitive)
    {
        if (!dev || !primitive)
        {
            return;
        }
        dev->submitOverlay(primitive);
    }

    RENDER_API void renderSubmitOverlays(RenderDevice* dev, const OverlayPrimitive* primitives, uint32_t count)
    {
        if (!dev || !primitives || count == 0)
        {
            return;
        }
        dev->submitOverlays(primitives, count);
    }

    RENDER_API void renderClearOverlays(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }
        dev->clearOverlays();
    }

    RENDER_API void renderClearOverlayGroup(RenderDevice* dev, OverlayGroup group)
    {
        if (!dev)
        {
            return;
        }
        dev->clearOverlayGroup(group);
    }

    // ==================== 场景环境 ====================

    RENDER_API void renderSetSceneEnvEx(RenderDevice* dev,
        const VertexP3C3* vertices,
        uint32_t vertexCount,
        const uint32_t* layerOffsets,
        uint32_t layerCount,
        const uint32_t* layerColors,
        const float* layerWidths,
        const bool* pixelFlags,
        const bool* triangleFlags,
        const float* zDepths)
    {
        if (!dev)
        {
            return;
        }
        dev->setSceneEnvEx(
            vertices, vertexCount, layerOffsets, layerCount, layerColors, layerWidths, pixelFlags, triangleFlags, zDepths);
    }

    RENDER_API void renderSetSceneEnvDirect(RenderDevice* dev, const SceneEnvGeometryDesc* desc)
    {
        if (!dev)
        {
            return;
        }
        dev->setSceneEnvDirect(desc);
    }

    // ==================== 位图 ====================

    // 单图便捷入口：固定 entityId=0，供旧调用方使用（等价 renderUpsertBitmap(dev,0,...)）
    RENDER_API void renderSetBitmap(RenderDevice* dev,
        const uint8_t* rgba,
        int32_t w,
        int32_t h,
        float tlX,
        float tlY,
        float trX,
        float trY,
        float blX,
        float blY,
        float brX,
        float brY)
    {
        if (!dev)
        {
            return;
        }
        const float corners[8] = {
            tlX,
            tlY,  // TL
            trX,
            trY,  // TR
            blX,
            blY,  // BL
            brX,
            brY  // BR
        };
        dev->bitmapRenderer.set(0, rgba, w, h, corners);
    }

    RENDER_API void renderClearBitmap(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }
        dev->bitmapRenderer.remove(0);
    }

    RENDER_API void renderUpsertBitmap(RenderDevice* dev,
        uint64_t entityId,
        const uint8_t* rgba,
        int32_t w,
        int32_t h,
        float tlX,
        float tlY,
        float trX,
        float trY,
        float blX,
        float blY,
        float brX,
        float brY)
    {
        if (!dev || entityId == 0)
        {
            return;
        }
        const float corners[8] = {
            tlX,
            tlY,  // TL
            trX,
            trY,  // TR
            blX,
            blY,  // BL
            brX,
            brY  // BR
        };
        dev->bitmapRenderer.set(entityId, rgba, w, h, corners);
    }

    RENDER_API void renderRemoveBitmap(RenderDevice* dev, uint64_t entityId)
    {
        if (!dev || entityId == 0)
        {
            return;
        }
        dev->bitmapRenderer.remove(entityId);
    }

    RENDER_API void renderClearBitmaps(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }
        dev->bitmapRenderer.clear();
    }

    // ==================== 新一代 Overlay Draw API ====================

    RENDER_API uint32_t renderSubmitOverlayDraw(RenderDevice* dev,
        const OverlayVertex* vertices, uint32_t vertexCount, const OverlayDrawRange* range)
    {
        if (!dev || !vertices || !range || vertexCount == 0 || range->vertexCount == 0)
        {
            return UINT32_MAX;
        }

        Render::core::OverlayQueue::DrawRange rangeCpp;
        rangeCpp.vertexOffset = 0; // 由内部分配
        rangeCpp.vertexCount = range->vertexCount;
        rangeCpp.topology = range->topology;
        rangeCpp.group = range->group;
        rangeCpp.zOrder = range->zOrder;
        rangeCpp.isTriangle = range->isTriangle != 0;

        return dev->overlayQueue.submit(
            reinterpret_cast<const Render::OverlayVertex*>(vertices),
            vertexCount,
            rangeCpp);
    }

    RENDER_API uint32_t renderSubmitOverlayDrawBatch(RenderDevice* dev,
        const OverlayDrawItem* items, uint32_t count)
    {
        if (!dev || !items || count == 0)
        {
            return UINT32_MAX;
        }

        // 转换为内部结构
        std::vector<Render::core::OverlayQueue::DrawItem> itemsCpp;
        itemsCpp.reserve(count);

        for (uint32_t i = 0; i < count; ++i)
        {
            const OverlayDrawItem& item = items[i];
            if (!item.vertices || item.vertexCount == 0)
            {
                continue;
            }

            Render::core::OverlayQueue::DrawItem itemCpp;
            itemCpp.vertices = reinterpret_cast<const Render::OverlayVertex*>(item.vertices);
            itemCpp.vertexCount = item.vertexCount;
            itemCpp.range.vertexOffset = 0;
            itemCpp.range.vertexCount = item.range.vertexCount;
            itemCpp.range.topology = item.range.topology;
            itemCpp.range.group = item.range.group;
            itemCpp.range.zOrder = item.range.zOrder;
            itemCpp.range.isTriangle = item.range.isTriangle != 0;

            itemsCpp.push_back(itemCpp);
        }

        if (itemsCpp.empty())
        {
            return UINT32_MAX;
        }

        return dev->overlayQueue.submitBatch(itemsCpp.data(), static_cast<uint32_t>(itemsCpp.size()));
    }

    RENDER_API void renderClearOverlayDrawGroup(RenderDevice* dev, uint32_t group)
    {
        if (!dev)
        {
            return;
        }
        dev->overlayQueue.clearGroup(group);
    }

    RENDER_API void renderClearOverlayDrawAll(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }
        dev->overlayQueue.clearAll();
    }
}  // extern "C"