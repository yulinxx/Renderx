#pragma once

#include "render_types.h"
#include <cstdint>

#ifndef RENDER_API
  #if defined(_WIN32) || defined(_WIN64)
    #ifdef RENDER_EXPORTS
      #define RENDER_API __declspec(dllexport)
    #else
      #define RENDER_API __declspec(dllimport)
    #endif
  #elif defined(__GNUC__) || defined(__clang__)
    #ifdef RENDER_EXPORTS
      #define RENDER_API __attribute__((visibility("default")))
    #else
      #define RENDER_API
    #endif
  #else
    #define RENDER_API
  #endif
#endif

namespace render {

struct RenderStats
{
    uint32_t entityCount;
    uint32_t visibleCount;
    uint32_t drawCallCount;
    uint32_t triangleCount;
    uint32_t lineCount;
    uint32_t pointCount;
    uint64_t gpuMemoryBytes;
};

typedef struct RenderDevice RenderDevice;

extern "C" {

RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc);
RENDER_API void          renderDestroyDevice(RenderDevice* dev);

RENDER_API void renderResize(RenderDevice* dev, uint32_t width, uint32_t height);

RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
                                    const VertexP3C3* vertices, uint32_t vertexCount,
                                    PrimitiveType type, uint16_t materialIdx);
RENDER_API void     renderModifyEntity(RenderDevice* dev, EntityId id,
                                       const VertexP3C3* vertices, uint32_t vertexCount,
                                       uint16_t materialIdx);
RENDER_API void     renderRemoveEntity(RenderDevice* dev, EntityId id);
RENDER_API void     renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible);

RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize);

RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
                                     const float* positions, const float* normals,
                                     const uint32_t* indices,
                                     uint32_t vertexCount, uint32_t indexCount);
RENDER_API void   renderUnregisterMesh(RenderDevice* dev, MeshId mesh);

RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
                                      const float modelMatrix[16], uint32_t materialIdx);
RENDER_API void     renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
                                         const float modelMatrix[16]);
RENDER_API void     renderRemoveInstance(RenderDevice* dev, uint32_t instanceId);

RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc);
RENDER_API void     renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc);

RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9],
                                float viewWidth, float viewHeight);
RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16],
                                const float projMatrix[16]);

RENDER_API void renderSetOverlay(RenderDevice* dev, const OverlayData* overlay);
RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts);

RENDER_API void renderSetPreviewLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA);
RENDER_API void renderSetControlLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA);
RENDER_API void renderSetPointMarkers(RenderDevice* dev, const float* worldPositions,
                                      uint32_t count, float markerSize,
                                      uint32_t fillColor, uint32_t borderColor);
RENDER_API void renderSetSelectionBox(RenderDevice* dev, const BBox2f* bbox, uint32_t colorRGBA);
RENDER_API void renderSetSelectionHandles(RenderDevice* dev, const float* worldPositions,
                                          uint32_t count, float handleSize,
                                          uint32_t fillColor, uint32_t borderColor);

RENDER_API void renderSetSceneEnv(RenderDevice* dev, const VertexP3C3* vertices,
                                  uint32_t vertexCount, const uint32_t* layerOffsets,
                                  uint32_t layerCount, const uint32_t* layerColors,
                                  const float* layerWidths);

RENDER_API void renderSetBitmap(RenderDevice* dev, const uint8_t* rgba, int32_t w, int32_t h,
                                float tlX, float tlY, float trX, float trY,
                                float blX, float blY, float brX, float brY);
RENDER_API void renderClearBitmap(RenderDevice* dev);

RENDER_API void renderFrame(RenderDevice* dev);
RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats);

RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev);
RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev);

RENDER_API void* renderGetNativeContext(RenderDevice* dev);

}

}
