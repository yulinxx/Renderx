#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>

namespace render {
namespace core {

class OverlayQueue {
public:
    struct OverlayVertex {
        float px, py, pz;
        float cr, cg, cb, ca;
    };

    static_assert(sizeof(OverlayVertex) == 28, "OverlayVertex must be 28 bytes");

    void initialize(rhi::IDevice* device);
    void shutdown();

    void setCrosshair(float worldX, float worldY, bool visible);
    void setSnapIndicator(float worldX, float worldY, bool visible,
                          const float color[4]);
    void setPreviewLines(const VertexP3C3* vertices, uint32_t count,
                         uint32_t colorRGBA);
    void setControlLines(const VertexP3C3* vertices, uint32_t count,
                         uint32_t colorRGBA);
    void setPointMarkers(const float* worldPositions, uint32_t count,
                         float markerSize, uint32_t fillColor,
                         uint32_t borderColor);
    void setSelectionBox(const BBox2f* bbox, uint32_t colorRGBA);
    void setSelectionHandles(const float* worldPositions, uint32_t count,
                             float handleSize, uint32_t fillColor,
                             uint32_t borderColor);

    void render(rhi::IDevice* device, const float viewMatrix[9]);

private:
    std::vector<OverlayVertex> m_crosshairVerts;
    std::vector<OverlayVertex> m_snapVerts;
    std::vector<OverlayVertex> m_previewVerts;
    std::vector<OverlayVertex> m_controlVerts;
    std::vector<OverlayVertex> m_markerVerts;
    std::vector<OverlayVertex> m_selectionBoxVerts;
    std::vector<OverlayVertex> m_handleVerts;

    rhi::IDevice*       m_device           = nullptr;
    rhi::BufferHandle   m_vertexBuffer     = rhi::NullHandle;
    rhi::PipelineHandle m_linePipeline      = {};
    rhi::PipelineHandle m_trianglePipeline  = {};
    uint32_t            m_vbCapacity        = 0;
    bool                m_dirty             = false;

    void buildMarkerQuad(OverlayVertex* out, float cx, float cy,
                         float halfSize, uint32_t fillColor);
    void buildMarkerBorder(OverlayVertex* out, float cx, float cy,
                           float halfSize, uint32_t borderColor);
    void uploadAndRender(rhi::IDevice* device, const OverlayVertex* data,
                         uint32_t vertexCount, PrimitiveType type);
};

} // namespace core
} // namespace render
