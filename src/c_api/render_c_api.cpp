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

#include <cstring>
#include <cassert>

namespace render {

struct RenderDevice
{
    rhi::IDevice*        rhiDevice      = nullptr;

    core::RenderWorld    world2D;
    core::BatchQueue     batchQueue;
    core::OverlayQueue   overlayQueue;
    core::MeshManager    meshManager;
    core::TextAtlas      textAtlas;
    core::SceneEnv       sceneEnv;

    ViewDesc2D           view2D         = {};
    ViewDesc3D           view3D         = {};
    OverlayData          overlay        = {};
    RenderStats          stats          = {};
    bool                 initialized    = false;
};

}

using namespace render;

extern "C" {

RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc)
{
    if (!desc) return nullptr;

    auto* dev = new RenderDevice();

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

    if (!dev->rhiDevice->initialize(desc->nativeWindowHandle, desc->width, desc->height))
    {
        delete dev;
        return nullptr;
    }

    dev->world2D.initialize();
    dev->batchQueue.initialize(dev->rhiDevice);
    dev->overlayQueue.initialize(dev->rhiDevice);
    dev->meshManager.initialize(dev->rhiDevice);
    dev->textAtlas.initialize(dev->rhiDevice);
    dev->sceneEnv.initialize(dev->rhiDevice);

    dev->initialized = true;
    return dev;
}

RENDER_API void renderDestroyDevice(RenderDevice* dev)
{
    if (!dev) return;

    dev->sceneEnv.shutdown();
    dev->textAtlas.shutdown();
    dev->meshManager.shutdown();
    dev->overlayQueue.shutdown();
    dev->batchQueue.shutdown();
    dev->world2D.shutdown();

    if (dev->rhiDevice)
    {
        dev->rhiDevice->shutdown();
        delete dev->rhiDevice;
    }

    delete dev;
}

RENDER_API void renderResize(RenderDevice* dev, uint32_t width, uint32_t height)
{
    if (!dev || !dev->rhiDevice) return;
    dev->rhiDevice->resize(width, height);
}

RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
                                    const VertexP3C3* vertices, uint32_t vertexCount,
                                    PrimitiveType type, uint16_t materialIdx)
{
    if (!dev) return 0;
    dev->world2D.addEntity(id, vertices, vertexCount, type, materialIdx);
    return 1;
}

RENDER_API void renderModifyEntity(RenderDevice* dev, EntityId id,
                                   const VertexP3C3* vertices, uint32_t vertexCount,
                                   uint16_t materialIdx)
{
    if (!dev) return;
    dev->world2D.modifyEntity(id, vertices, vertexCount, materialIdx);
}

RENDER_API void renderRemoveEntity(RenderDevice* dev, EntityId id)
{
    if (!dev) return;
    dev->world2D.removeEntity(id);
}

RENDER_API void renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible)
{
    if (!dev) return;
    dev->world2D.setEntityVisibility(id, visible != 0);
}

RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize)
{
    if (!dev || !packet) return;

    const uint8_t* ptr = static_cast<const uint8_t*>(packet);
    const uint8_t* end = ptr + packetSize;

    uint32_t updateCount;
    std::memcpy(&updateCount, ptr, 4);
    ptr += 8;

    for (uint32_t i = 0; i < updateCount && ptr < end; ++i)
    {
        EntityUpdate upd;
        std::memcpy(&upd, ptr, sizeof(EntityUpdate));
        ptr += sizeof(EntityUpdate);

        const VertexP3C3* verts = reinterpret_cast<const VertexP3C3*>(ptr);
        ptr += upd.vertexCount * sizeof(VertexP3C3);

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

RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
                                     const float* positions, const float* normals,
                                     const uint32_t* indices,
                                     uint32_t vertexCount, uint32_t indexCount)
{
    if (!dev) return INVALID_MESH_ID;
    return dev->meshManager.registerMesh(positions, normals, indices, vertexCount, indexCount);
}

RENDER_API void renderUnregisterMesh(RenderDevice* dev, MeshId mesh)
{
    if (!dev) return;
    dev->meshManager.unregisterMesh(mesh);
}

RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
                                      const float modelMatrix[16], uint32_t materialIdx)
{
    if (!dev) return UINT32_MAX;
    return dev->meshManager.addInstance(mesh, modelMatrix, materialIdx);
}

RENDER_API void renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
                                     const float modelMatrix[16])
{
    if (!dev) return;
    dev->meshManager.modifyInstance(instanceId, modelMatrix);
}

RENDER_API void renderRemoveInstance(RenderDevice* dev, uint32_t instanceId)
{
    if (!dev) return;
    dev->meshManager.removeInstance(instanceId);
}

RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc)
{
    if (!dev) return 0;
    return dev->world2D.addMaterial(desc);
}

RENDER_API void renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc)
{
    if (!dev) return;
    dev->world2D.updateMaterial(idx, desc);
}

RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9],
                                float viewWidth, float viewHeight)
{
    if (!dev) return;
    std::memcpy(dev->view2D.viewMatrix, viewMatrix, 9 * sizeof(float));
    dev->view2D.viewWidth = viewWidth;
    dev->view2D.viewHeight = viewHeight;
}

RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16],
                                const float projMatrix[16])
{
    if (!dev) return;
    std::memcpy(dev->view3D.viewMatrix, viewMatrix, 16 * sizeof(float));
    std::memcpy(dev->view3D.projMatrix, projMatrix, 16 * sizeof(float));
}

RENDER_API void renderSetOverlay(RenderDevice* dev, const OverlayData* overlay)
{
    if (!dev || !overlay) return;
    dev->overlay = *overlay;
    dev->overlayQueue.setCrosshair(overlay->crosshairWorld[0], overlay->crosshairWorld[1],
                                   overlay->crosshairVisible != 0);
    dev->overlayQueue.setSnapIndicator(overlay->snapWorld[0], overlay->snapWorld[1],
                                       overlay->snapVisible != 0, overlay->snapColor);
}

RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts)
{
    if (!dev) return;
    dev->textAtlas.renderText(texts, dev->view2D.viewMatrix, dev->rhiDevice);
}

RENDER_API void renderSetPreviewLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA)
{
    if (!dev) return;
    dev->overlayQueue.setPreviewLines(vertices, vertexCount, colorRGBA);
}

RENDER_API void renderSetControlLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA)
{
    if (!dev) return;
    dev->overlayQueue.setControlLines(vertices, vertexCount, colorRGBA);
}

RENDER_API void renderSetPointMarkers(RenderDevice* dev, const float* worldPositions,
                                      uint32_t count, float markerSize,
                                      uint32_t fillColor, uint32_t borderColor)
{
    if (!dev) return;
    dev->overlayQueue.setPointMarkers(worldPositions, count, markerSize, fillColor, borderColor);
}

RENDER_API void renderSetSelectionBox(RenderDevice* dev, const BBox2f* bbox, uint32_t colorRGBA)
{
    if (!dev) return;
    dev->overlayQueue.setSelectionBox(bbox, colorRGBA);
}

RENDER_API void renderSetSelectionHandles(RenderDevice* dev, const float* worldPositions,
                                          uint32_t count, float handleSize,
                                          uint32_t fillColor, uint32_t borderColor)
{
    if (!dev) return;
    dev->overlayQueue.setSelectionHandles(worldPositions, count, handleSize, fillColor, borderColor);
}

RENDER_API void renderSetSceneEnv(RenderDevice* dev, const VertexP3C3* vertices,
                                  uint32_t vertexCount, const uint32_t* layerOffsets,
                                  uint32_t layerCount, const uint32_t* layerColors,
                                  const float* layerWidths)
{
    if (!dev) return;
    dev->sceneEnv.setGeometry(vertices, vertexCount, layerOffsets, layerCount, layerColors, layerWidths);
}

RENDER_API void renderSetBitmap(RenderDevice* dev, const uint8_t* rgba, int32_t w, int32_t h,
                                float tlX, float tlY, float trX, float trY,
                                float blX, float blY, float brX, float brY)
{
}

RENDER_API void renderClearBitmap(RenderDevice* dev)
{
}

RENDER_API void renderFrame(RenderDevice* dev)
{
    if (!dev || !dev->rhiDevice) return;

    auto* rhi = dev->rhiDevice;
    rhi->beginFrame();
    rhi->setClearColor(0.93f, 0.93f, 0.93f, 1.0f);
    rhi->clear(0x00004000 | 0x00000100);
    rhi->enableDepthTest(false);
    rhi->enableBlend(true);

    dev->world2D.update();

    uint32_t visibleIndices[8192];
    uint32_t visibleCount = 0;
    dev->world2D.queryVisible(dev->view2D.viewMatrix, dev->view2D.viewWidth,
                              dev->view2D.viewHeight, visibleIndices, &visibleCount, 8192);

    dev->sceneEnv.render(rhi, dev->view2D.viewMatrix);

    dev->batchQueue.submit(visibleIndices, visibleCount, dev->world2D);
    dev->batchQueue.render(rhi);

    dev->overlayQueue.render(rhi, dev->view2D.viewMatrix);

    dev->meshManager.update();
    dev->meshManager.render(rhi, dev->view3D.viewMatrix, dev->view3D.projMatrix);

    rhi->endFrame();
    rhi->present();

    dev->stats.entityCount = dev->world2D.getEntityCount();
    dev->stats.visibleCount = visibleCount;
    dev->stats.gpuMemoryBytes = rhi->getGPUMemoryUsage();
}

RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats)
{
    if (!dev || !stats) return;
    *stats = dev->stats;
}

RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev)
{
    if (!dev) return 0;
    return dev->world2D.getEntityCount();
}

RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev)
{
    if (!dev || !dev->rhiDevice) return 0;
    return dev->rhiDevice->getGPUMemoryUsage();
}

RENDER_API void* renderGetNativeContext(RenderDevice* dev)
{
    if (!dev || !dev->rhiDevice) return nullptr;
    return dev->rhiDevice->getNativeContext();
}

}
