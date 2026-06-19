#include "overlay_queue.h"
#include <cstring>
#include <cmath>

namespace render {
namespace core {

static void unpackRGBA(uint32_t rgba, float& r, float& g, float& b, float& a) {
    r = ((rgba >> 0) & 0xFF) / 255.0f;
    g = ((rgba >> 8) & 0xFF) / 255.0f;
    b = ((rgba >> 16) & 0xFF) / 255.0f;
    a = ((rgba >> 24) & 0xFF) / 255.0f;
}

static OverlayQueue::OverlayVertex makeVert(float x, float y, float z, float r, float g, float b, float a) {
    OverlayQueue::OverlayVertex v;
    v.px = x; v.py = y; v.pz = z;
    v.cr = r; v.cg = g; v.cb = b; v.ca = a;
    return v;
}

void OverlayQueue::initialize(rhi::IDevice* device) {
    m_device = device;

    rhi::PipelineDesc lineDesc;
    lineDesc.topology       = rhi::PrimitiveTopology::LineList;
    lineDesc.vertexShader   = "overlay_vert";
    lineDesc.fragmentShader = "overlay_frag";
    lineDesc.computeShader  = nullptr;
    lineDesc.depthTest      = false;
    lineDesc.depthWrite     = false;
    lineDesc.blendEnable    = true;
    lineDesc.srcBlend       = rhi::BlendFactor::SrcAlpha;
    lineDesc.dstBlend       = rhi::BlendFactor::OneMinusSrcAlpha;
    lineDesc.depthFunc      = rhi::CompareFunc::Always;
    m_linePipeline = device->createPipeline(lineDesc);

    rhi::PipelineDesc triDesc = lineDesc;
    triDesc.topology = rhi::PrimitiveTopology::TriangleList;
    m_trianglePipeline = device->createPipeline(triDesc);

    rhi::BufferDesc vbDesc;
    vbDesc.size       = 4096 * sizeof(OverlayVertex);
    vbDesc.usage      = rhi::BufferUsage::Vertex;
    vbDesc.memory     = rhi::MemoryType::GPU_CPU_Coherent;
    vbDesc.debugName  = "OverlayQueue_VB";
    m_vertexBuffer    = device->createBuffer(vbDesc);
    m_vbCapacity      = 4096;
}

void OverlayQueue::shutdown() {
    if (m_vertexBuffer != rhi::NullHandle) {
        m_device->destroyBuffer(m_vertexBuffer);
        m_vertexBuffer = rhi::NullHandle;
    }
    if (m_linePipeline != rhi::NullHandle) {
        m_device->destroyPipeline(m_linePipeline);
        m_linePipeline = {};
    }
    if (m_trianglePipeline != rhi::NullHandle) {
        m_device->destroyPipeline(m_trianglePipeline);
        m_trianglePipeline = {};
    }

    m_crosshairVerts.clear();
    m_snapVerts.clear();
    m_previewVerts.clear();
    m_controlVerts.clear();
    m_markerVerts.clear();
    m_selectionBoxVerts.clear();
    m_handleVerts.clear();

    m_vbCapacity = 0;
    m_dirty = false;
    m_device = nullptr;
}

void OverlayQueue::setCrosshair(float worldX, float worldY, bool visible) {
    m_crosshairVerts.clear();
    if (!visible) return;

    const float len = 20.0f;
    float z = 0.0f;
    float r = 1.0f, g = 1.0f, b = 1.0f, a = 0.8f;

    m_crosshairVerts.push_back(makeVert(worldX - len, worldY, z, r, g, b, a));
    m_crosshairVerts.push_back(makeVert(worldX + len, worldY, z, r, g, b, a));
    m_crosshairVerts.push_back(makeVert(worldX, worldY - len, z, r, g, b, a));
    m_crosshairVerts.push_back(makeVert(worldX, worldY + len, z, r, g, b, a));

    m_dirty = true;
}

void OverlayQueue::setSnapIndicator(float worldX, float worldY, bool visible,
                                    const float color[4]) {
    m_snapVerts.clear();
    if (!visible) return;

    const float radius = 8.0f;
    float z = 0.0f;
    float r = color[0], g = color[1], b = color[2], a = color[3];

    const int kSegments = 16;
    for (int i = 0; i < kSegments; ++i) {
        float a0 = 2.0f * 3.14159265f * i / kSegments;
        float a1 = 2.0f * 3.14159265f * (i + 1) / kSegments;
        m_snapVerts.push_back(makeVert(worldX + radius * std::cos(a0),
                                       worldY + radius * std::sin(a0), z, r, g, b, a));
        m_snapVerts.push_back(makeVert(worldX + radius * std::cos(a1),
                                       worldY + radius * std::sin(a1), z, r, g, b, a));
    }

    m_dirty = true;
}

void OverlayQueue::setPreviewLines(const VertexP3C3* vertices, uint32_t count,
                                   uint32_t colorRGBA) {
    m_previewVerts.clear();
    if (!vertices || count == 0) return;

    float r, g, b, a;
    unpackRGBA(colorRGBA, r, g, b, a);

    m_previewVerts.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        m_previewVerts[i] = makeVert(vertices[i].px, vertices[i].py, vertices[i].pz,
                                     r, g, b, a);
    }

    m_dirty = true;
}

void OverlayQueue::setControlLines(const VertexP3C3* vertices, uint32_t count,
                                   uint32_t colorRGBA) {
    m_controlVerts.clear();
    if (!vertices || count == 0) return;

    float r, g, b, a;
    unpackRGBA(colorRGBA, r, g, b, a);

    m_controlVerts.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        m_controlVerts[i] = makeVert(vertices[i].px, vertices[i].py, vertices[i].pz,
                                     r, g, b, a);
    }

    m_dirty = true;
}

void OverlayQueue::setPointMarkers(const float* worldPositions, uint32_t count,
                                   float markerSize, uint32_t fillColor,
                                   uint32_t borderColor) {
    m_markerVerts.clear();
    if (!worldPositions || count == 0) return;

    float half = markerSize * 0.5f;

    m_markerVerts.resize(count * 14);
    OverlayVertex* ptr = m_markerVerts.data();
    for (uint32_t i = 0; i < count; ++i) {
        float cx = worldPositions[i * 2 + 0];
        float cy = worldPositions[i * 2 + 1];
        buildMarkerQuad(ptr, cx, cy, half, fillColor);
        ptr += 6;
        buildMarkerBorder(ptr, cx, cy, half, borderColor);
        ptr += 8;
    }

    m_dirty = true;
}

void OverlayQueue::setSelectionBox(const BBox2f* bbox, uint32_t colorRGBA) {
    m_selectionBoxVerts.clear();
    if (!bbox) return;

    float r, g, b, a;
    unpackRGBA(colorRGBA, r, g, b, a);

    float z = 0.0f;
    float minX = bbox->minX, minY = bbox->minY;
    float maxX = bbox->maxX, maxY = bbox->maxY;

    m_selectionBoxVerts.push_back(makeVert(minX, minY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(maxX, minY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(maxX, minY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(maxX, maxY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(maxX, maxY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(minX, maxY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(minX, maxY, z, r, g, b, a));
    m_selectionBoxVerts.push_back(makeVert(minX, minY, z, r, g, b, a));

    m_dirty = true;
}

void OverlayQueue::setSelectionHandles(const float* worldPositions, uint32_t count,
                                       float handleSize, uint32_t fillColor,
                                       uint32_t borderColor) {
    m_handleVerts.clear();
    if (!worldPositions || count == 0) return;

    float half = handleSize * 0.5f;

    m_handleVerts.resize(count * 14);
    OverlayVertex* ptr = m_handleVerts.data();
    for (uint32_t i = 0; i < count; ++i) {
        float cx = worldPositions[i * 2 + 0];
        float cy = worldPositions[i * 2 + 1];
        buildMarkerQuad(ptr, cx, cy, half, fillColor);
        ptr += 6;
        buildMarkerBorder(ptr, cx, cy, half, borderColor);
        ptr += 8;
    }

    m_dirty = true;
}

void OverlayQueue::render(rhi::IDevice* device, const float viewMatrix[9]) {
    uint32_t totalVerts =
        static_cast<uint32_t>(m_selectionBoxVerts.size()) +
        static_cast<uint32_t>(m_controlVerts.size()) +
        static_cast<uint32_t>(m_previewVerts.size()) +
        static_cast<uint32_t>(m_markerVerts.size()) +
        static_cast<uint32_t>(m_handleVerts.size()) +
        static_cast<uint32_t>(m_crosshairVerts.size()) +
        static_cast<uint32_t>(m_snapVerts.size());

    if (totalVerts == 0) return;

    if (totalVerts > m_vbCapacity) {
        if (m_vertexBuffer != rhi::NullHandle)
            device->destroyBuffer(m_vertexBuffer);

        uint32_t newCap = m_vbCapacity;
        if (newCap == 0) newCap = 4096;
        while (newCap < totalVerts) newCap *= 2;

        rhi::BufferDesc desc;
        desc.size       = newCap * sizeof(OverlayVertex);
        desc.usage      = rhi::BufferUsage::Vertex;
        desc.memory     = rhi::MemoryType::GPU_CPU_Coherent;
        desc.debugName  = "OverlayQueue_VB";
        m_vertexBuffer  = device->createBuffer(desc);
        m_vbCapacity    = newCap;
    }

    std::vector<OverlayVertex> merged;
    merged.reserve(totalVerts);

    uint32_t selStart     = static_cast<uint32_t>(merged.size());
    uint32_t selCount     = static_cast<uint32_t>(m_selectionBoxVerts.size());
    merged.insert(merged.end(), m_selectionBoxVerts.begin(), m_selectionBoxVerts.end());

    uint32_t ctrlStart    = static_cast<uint32_t>(merged.size());
    uint32_t ctrlCount    = static_cast<uint32_t>(m_controlVerts.size());
    merged.insert(merged.end(), m_controlVerts.begin(), m_controlVerts.end());

    uint32_t prevStart    = static_cast<uint32_t>(merged.size());
    uint32_t prevCount    = static_cast<uint32_t>(m_previewVerts.size());
    merged.insert(merged.end(), m_previewVerts.begin(), m_previewVerts.end());

    uint32_t markerStart = static_cast<uint32_t>(merged.size());
    uint32_t markerCount = static_cast<uint32_t>(m_markerVerts.size());
    merged.insert(merged.end(), m_markerVerts.begin(), m_markerVerts.end());

    uint32_t handleStart = static_cast<uint32_t>(merged.size());
    uint32_t handleCount = static_cast<uint32_t>(m_handleVerts.size());
    merged.insert(merged.end(), m_handleVerts.begin(), m_handleVerts.end());

    uint32_t crossStart  = static_cast<uint32_t>(merged.size());
    uint32_t crossCount  = static_cast<uint32_t>(m_crosshairVerts.size());
    merged.insert(merged.end(), m_crosshairVerts.begin(), m_crosshairVerts.end());

    uint32_t snapStart   = static_cast<uint32_t>(merged.size());
    uint32_t snapCount   = static_cast<uint32_t>(m_snapVerts.size());
    merged.insert(merged.end(), m_snapVerts.begin(), m_snapVerts.end());

    device->uploadBuffer(m_vertexBuffer, 0,
                         merged.size() * sizeof(OverlayVertex),
                         merged.data());

    if (selCount > 0) {
        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 selStart * sizeof(OverlayVertex));
        device->draw(selCount, 1, 0, 0);
    }
    if (ctrlCount > 0) {
        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 ctrlStart * sizeof(OverlayVertex));
        device->draw(ctrlCount, 1, 0, 0);
    }
    if (prevCount > 0) {
        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 prevStart * sizeof(OverlayVertex));
        device->draw(prevCount, 1, 0, 0);
    }
    if (markerCount > 0) {
        uint32_t triCount = (markerCount / 14) * 6;
        uint32_t lineCount = (markerCount / 14) * 8;

        device->bindPipeline(m_trianglePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 markerStart * sizeof(OverlayVertex));
        device->draw(triCount, 1, 0, 0);

        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 (markerStart + triCount) * sizeof(OverlayVertex));
        device->draw(lineCount, 1, 0, 0);
    }
    if (handleCount > 0) {
        uint32_t triCount = (handleCount / 14) * 6;
        uint32_t lineCount = (handleCount / 14) * 8;

        device->bindPipeline(m_trianglePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 handleStart * sizeof(OverlayVertex));
        device->draw(triCount, 1, 0, 0);

        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 (handleStart + triCount) * sizeof(OverlayVertex));
        device->draw(lineCount, 1, 0, 0);
    }
    if (crossCount > 0) {
        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 crossStart * sizeof(OverlayVertex));
        device->draw(crossCount, 1, 0, 0);
    }
    if (snapCount > 0) {
        device->bindPipeline(m_linePipeline);
        device->bindVertexBuffer(0, m_vertexBuffer,
                                 snapStart * sizeof(OverlayVertex));
        device->draw(snapCount, 1, 0, 0);
    }

    m_dirty = false;
}

void OverlayQueue::buildMarkerQuad(OverlayVertex* out, float cx, float cy,
                                   float halfSize, uint32_t fillColor) {
    float r, g, b, a;
    unpackRGBA(fillColor, r, g, b, a);

    float x0 = cx - halfSize, x1 = cx + halfSize;
    float y0 = cy - halfSize, y1 = cy + halfSize;
    float z = 0.0f;

    out[0] = makeVert(x0, y0, z, r, g, b, a);
    out[1] = makeVert(x1, y0, z, r, g, b, a);
    out[2] = makeVert(x1, y1, z, r, g, b, a);

    out[3] = makeVert(x0, y0, z, r, g, b, a);
    out[4] = makeVert(x1, y1, z, r, g, b, a);
    out[5] = makeVert(x0, y1, z, r, g, b, a);
}

void OverlayQueue::buildMarkerBorder(OverlayVertex* out, float cx, float cy,
                                     float halfSize, uint32_t borderColor) {
    float r, g, b, a;
    unpackRGBA(borderColor, r, g, b, a);

    float x0 = cx - halfSize, x1 = cx + halfSize;
    float y0 = cy - halfSize, y1 = cy + halfSize;
    float z = 0.0f;

    out[0] = makeVert(x0, y0, z, r, g, b, a);
    out[1] = makeVert(x1, y0, z, r, g, b, a);

    out[2] = makeVert(x1, y0, z, r, g, b, a);
    out[3] = makeVert(x1, y1, z, r, g, b, a);

    out[4] = makeVert(x1, y1, z, r, g, b, a);
    out[5] = makeVert(x0, y1, z, r, g, b, a);

    out[6] = makeVert(x0, y1, z, r, g, b, a);
    out[7] = makeVert(x0, y0, z, r, g, b, a);
}

void OverlayQueue::uploadAndRender(rhi::IDevice* device, const OverlayVertex* data,
                                   uint32_t vertexCount, PrimitiveType type) {
    if (vertexCount == 0) return;

    device->uploadBuffer(m_vertexBuffer, 0,
                         vertexCount * sizeof(OverlayVertex), data);

    if (type == PrimitiveType::TriangleList) {
        device->bindPipeline(m_trianglePipeline);
    } else {
        device->bindPipeline(m_linePipeline);
    }

    device->bindVertexBuffer(0, m_vertexBuffer, 0);
    device->draw(vertexCount, 1, 0, 0);
}

} // namespace core
} // namespace render
