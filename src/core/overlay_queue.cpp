#include "overlay_queue.h"
#include "command_encoder.h"
#include "render/render_types.h"
#include "Log/SyLogger.h"
#include <cstring>
#include <cmath>

namespace render
{
    namespace core
    {
        static void unpackRGBA(uint32_t rgba, float& r, float& g, float& b, float& a)
        {
            r = ((rgba >> 0) & 0xFF) / 255.0f;
            g = ((rgba >> 8) & 0xFF) / 255.0f;
            b = ((rgba >> 16) & 0xFF) / 255.0f;
            a = ((rgba >> 24) & 0xFF) / 255.0f;
        }

        static OverlayQueue::OverlayVertex makeVert(float x, float y, float z, float r, float g, float b, float a)
        {
            OverlayQueue::OverlayVertex v;
            v.px = x; v.py = y; v.pz = z;
            v.cr = r; v.cg = g; v.cb = b; v.ca = a;
            return v;
        }

        void OverlayQueue::initialize(rhi::IDevice* device)
        {
            m_device = device;

            rhi::PipelineDesc lineDesc;
            lineDesc.topology = rhi::PrimitiveTopology::LineList;
            lineDesc.vertexShader = "overlay_vert";
            lineDesc.fragmentShader = "overlay_frag";
            lineDesc.computeShader = nullptr;
            lineDesc.vertexFormat = rhi::VertexFormat::P3C4;
            lineDesc.depthTest = false;
            lineDesc.depthWrite = false;
            lineDesc.blendEnable = true;
            lineDesc.srcBlend = rhi::BlendFactor::SrcAlpha;
            lineDesc.dstBlend = rhi::BlendFactor::OneMinusSrcAlpha;
            lineDesc.depthFunc = rhi::CompareFunc::Always;
            m_linePipeline = device->createPipeline(lineDesc);

            rhi::PipelineDesc triDesc = lineDesc;
            triDesc.topology = rhi::PrimitiveTopology::TriangleList;
            m_trianglePipeline = device->createPipeline(triDesc);

            rhi::BufferDesc vbDesc;
            vbDesc.size = 4096 * sizeof(OverlayVertex);
            vbDesc.usage = rhi::BufferUsage::Vertex;
            vbDesc.memory = rhi::MemoryType::GPU_CPU_Coherent;
            vbDesc.debugName = "OverlayQueue_VB";
            m_vertexBuffer = device->createBuffer(vbDesc);
            m_vbCapacity = 4096;
        }

        void OverlayQueue::shutdown()
        {
            if (m_vertexBuffer != rhi::NullHandle)
            {
                m_device->destroyBuffer(m_vertexBuffer);
                m_vertexBuffer = rhi::NullHandle;
            }
            if (m_linePipeline != rhi::NullHandle)
            {
                m_device->destroyPipeline(m_linePipeline);
                m_linePipeline = {};
            }
            if (m_trianglePipeline != rhi::NullHandle)
            {
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
            m_selRectFillVerts.clear();
            m_selRectBorderVerts.clear();
            m_markerVertsPerItem = 14;
            m_handleVertsPerItem = 14;

            m_vbCapacity = 0;
            m_dirty = false;
            m_device = nullptr;
        }

        void OverlayQueue::setCrosshair(float worldX, float worldY, bool visible)
        {
            m_crosshairVerts.clear();
            if (!visible)
            {
                m_dirty = true;
                return;
            }

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
            const float color[4])
        {
            m_snapVerts.clear();
            if (!visible)
            {
                m_dirty = true;
                return;
            }

            const float radius = 8.0f;
            float z = 0.0f;
            float r = color[0], g = color[1], b = color[2], a = color[3];

            const int kSegments = 16;
            for (int i = 0; i < kSegments; ++i)
            {
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
            uint32_t colorRGBA)
        {
            m_previewVerts.clear();
            if (!vertices || count == 0)
            {
                m_dirty = true;
                return;
            }

            // 使用逐顶点颜色，保留 VertexP3C3 中的颜色信息
            // 选择轮廓等需要不同颜色的覆盖层依赖此行为
            m_previewVerts.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                m_previewVerts[i] = makeVert(vertices[i].px, vertices[i].py, vertices[i].pz,
                    vertices[i].cr, vertices[i].cg, vertices[i].cb, 1.0f);
            }

            m_dirty = true;
        }

        void OverlayQueue::setControlLines(const VertexP3C3* vertices, uint32_t count,
            uint32_t colorRGBA)
        {
            m_controlVerts.clear();
            if (!vertices || count == 0)
            {
                m_dirty = true;
                return;
            }

            // 使用逐顶点颜色，保留 VertexP3C3 中的颜色信息
            // 选择轮廓等需要不同颜色的覆盖层依赖此行为
            m_controlVerts.resize(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                m_controlVerts[i] = makeVert(vertices[i].px, vertices[i].py, vertices[i].pz,
                    vertices[i].cr, vertices[i].cg, vertices[i].cb, 1.0f);
            }

            m_dirty = true;
        }

        void OverlayQueue::setPointMarkers(const float* worldPositions, uint32_t count,
            float markerSize, uint32_t fillColor,
            uint32_t borderColor)
        {
            m_markerVerts.clear();
            m_unifiedVerts.clear();
            m_unifiedRanges.clear();
            if (!worldPositions || count == 0)
            {
                m_dirty = true;
                return;
            }

            float half = markerSize * 0.5f;
            float inner = std::max(half - 1.0f, half * 0.72f);

            // 检查填充色是否透明：alpha=0 时只渲染边框（8顶点），不渲染填充四边形
            uint8_t fillAlpha = (fillColor >> 24) & 0xFF;
            bool hasFill = (fillAlpha != 0);
            m_markerVertsPerItem = hasFill ? 14 : 8;

            m_markerVerts.resize(count * m_markerVertsPerItem);
            OverlayVertex* ptr = m_markerVerts.data();
            for (uint32_t i = 0; i < count; ++i)
            {
                float cx = worldPositions[i * 2 + 0];
                float cy = worldPositions[i * 2 + 1];
                if (hasFill)
                {
                    buildMarkerQuad(ptr, cx, cy, inner, fillColor);
                    ptr += 6;
                }
                buildMarkerBorder(ptr, cx, cy, half, borderColor);
                ptr += 8;
            }

            m_dirty = true;
        }

        void OverlayQueue::setSelectionBox(const BBox2f* bbox, uint32_t colorRGBA)
        {
            m_selectionBoxVerts.clear();
            m_unifiedVerts.clear();
            m_unifiedRanges.clear();
            if (!bbox)
            {
                m_dirty = true;
                return;
            }

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
            uint32_t borderColor)
        {
            m_handleVerts.clear();
            m_unifiedVerts.clear();
            m_unifiedRanges.clear();
            if (!worldPositions || count == 0)
            {
                m_dirty = true;
                return;
            }

            float half = handleSize * 0.5f;
            float inner = std::max(half - 1.0f, half * 0.72f);

            uint8_t fillAlpha = (fillColor >> 24) & 0xFF;
            bool hasFill = (fillAlpha != 0);
            m_handleVertsPerItem = hasFill ? 14 : 8;

            m_handleVerts.resize(count * m_handleVertsPerItem);
            OverlayVertex* ptr = m_handleVerts.data();
            for (uint32_t i = 0; i < count; ++i)
            {
                float cx = worldPositions[i * 2 + 0];
                float cy = worldPositions[i * 2 + 1];
                if (hasFill)
                {
                    buildMarkerQuad(ptr, cx, cy, inner, fillColor);
                    ptr += 6;
                }
                buildMarkerBorder(ptr, cx, cy, half, borderColor);
                ptr += 8;
            }

            m_dirty = true;
        }

        void OverlayQueue::setSelectionRect(const BBox2f* bbox, uint32_t fillColor,
            uint32_t borderColor)
        {
            m_selRectFillVerts.clear();
            m_selRectBorderVerts.clear();
            m_unifiedVerts.clear();
            m_unifiedRanges.clear();
            if (!bbox)
            {
                m_dirty = true;
                return;
            }

            float z = 0.0f;
            float minX = bbox->minX, minY = bbox->minY;
            float maxX = bbox->maxX, maxY = bbox->maxY;

            // 填充（2个三角形共6顶点）
            uint8_t fillAlpha = (fillColor >> 24) & 0xFF;
            if (fillAlpha != 0)
            {
                float fr, fg, fb, fa;
                unpackRGBA(fillColor, fr, fg, fb, fa);
                m_selRectFillVerts.resize(6);
                m_selRectFillVerts[0] = makeVert(minX, minY, z, fr, fg, fb, fa);
                m_selRectFillVerts[1] = makeVert(maxX, minY, z, fr, fg, fb, fa);
                m_selRectFillVerts[2] = makeVert(maxX, maxY, z, fr, fg, fb, fa);
                m_selRectFillVerts[3] = makeVert(minX, minY, z, fr, fg, fb, fa);
                m_selRectFillVerts[4] = makeVert(maxX, maxY, z, fr, fg, fb, fa);
                m_selRectFillVerts[5] = makeVert(minX, maxY, z, fr, fg, fb, fa);
            }

            // 边框（4条线段共8顶点）
            float br, bg, bb, ba;
            unpackRGBA(borderColor, br, bg, bb, ba);
            m_selRectBorderVerts.resize(8);
            m_selRectBorderVerts[0] = makeVert(minX, minY, z, br, bg, bb, ba);
            m_selRectBorderVerts[1] = makeVert(maxX, minY, z, br, bg, bb, ba);
            m_selRectBorderVerts[2] = makeVert(maxX, minY, z, br, bg, bb, ba);
            m_selRectBorderVerts[3] = makeVert(maxX, maxY, z, br, bg, bb, ba);
            m_selRectBorderVerts[4] = makeVert(maxX, maxY, z, br, bg, bb, ba);
            m_selRectBorderVerts[5] = makeVert(minX, maxY, z, br, bg, bb, ba);
            m_selRectBorderVerts[6] = makeVert(minX, maxY, z, br, bg, bb, ba);
            m_selRectBorderVerts[7] = makeVert(minX, minY, z, br, bg, bb, ba);

            m_dirty = true;
        }

        void OverlayQueue::submitOverlay(const OverlayPrimitive* primitive)
        {
            if (!primitive || !primitive->payload)
                return;

            uint32_t start = static_cast<uint32_t>(m_unifiedVerts.size());
            uint32_t count = 0;
            uint32_t isTriangle = 0;
            float z = 0.0f;

            switch (primitive->kind)
            {
                case OverlayPrimitiveKind::LineList:
                {
                    const auto* desc = static_cast<const OverlayPolylineDesc*>(primitive->payload);
                    if (!desc || !desc->vertices || desc->vertexCount == 0)
                        break;
                    count = desc->vertexCount;
                    m_unifiedVerts.reserve(start + count);
                    if (desc->usePerVertexColor && desc->colors)
                    {
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            m_unifiedVerts.push_back(makeVert(
                                desc->vertices[i * 3 + 0],
                                desc->vertices[i * 3 + 1],
                                desc->vertices[i * 3 + 2],
                                desc->colors[i * 3 + 0],
                                desc->colors[i * 3 + 1],
                                desc->colors[i * 3 + 2],
                                1.0f));
                        }
                    }
                    else
                    {
                        float r, g, b, a;
                        unpackRGBA(primitive->style.borderColor, r, g, b, a);
                        for (uint32_t i = 0; i < count; ++i)
                        {
                            m_unifiedVerts.push_back(makeVert(
                                desc->vertices[i * 3 + 0],
                                desc->vertices[i * 3 + 1],
                                desc->vertices[i * 3 + 2],
                                r, g, b, a));
                        }
                    }
                    break;
                }
                case OverlayPrimitiveKind::Rect:
                {
                    const auto* desc = static_cast<const OverlayRectDesc*>(primitive->payload);
                    if (!desc)
                        break;
                    float r, g, b, a;
                    unpackRGBA(primitive->style.borderColor, r, g, b, a);
                    count = 8;
                    m_unifiedVerts.reserve(start + count);
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->minY, z, r, g, b, a));
                    break;
                }
                case OverlayPrimitiveKind::FilledRect:
                {
                    const auto* desc = static_cast<const OverlayRectDesc*>(primitive->payload);
                    if (!desc)
                        break;
                    float r, g, b, a;
                    unpackRGBA(primitive->style.fillColor, r, g, b, a);
                    count = 6;
                    isTriangle = 1;
                    m_unifiedVerts.reserve(start + count);
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->minY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->maxX, desc->maxY, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(desc->minX, desc->maxY, z, r, g, b, a));
                    break;
                }
                case OverlayPrimitiveKind::Points:
                {
                    const auto* desc = static_cast<const OverlayMarkerSetDesc*>(primitive->payload);
                    if (!desc || !desc->positions || desc->count == 0)
                        break;
                    float half = primitive->style.pointSize * 0.5f;
                    float inner = std::max(half - 1.0f, half * 0.72f);
                    uint8_t fillAlpha = (primitive->style.fillColor >> 24) & 0xFF;
                    bool hasFill = (fillAlpha != 0);
                    uint32_t vertsPerItem = hasFill ? 14 : 8;
                    count = desc->count * vertsPerItem;
                    m_unifiedVerts.reserve(start + count);
                    for (uint32_t i = 0; i < desc->count; ++i)
                    {
                        float cx = desc->positions[i * 2 + 0];
                        float cy = desc->positions[i * 2 + 1];
                        if (hasFill)
                        {
                            m_unifiedVerts.resize(m_unifiedVerts.size() + 6);
                            buildMarkerQuad(&m_unifiedVerts[m_unifiedVerts.size() - 6], cx, cy, inner, primitive->style.fillColor);
                        }
                        m_unifiedVerts.resize(m_unifiedVerts.size() + 8);
                        buildMarkerBorder(&m_unifiedVerts[m_unifiedVerts.size() - 8], cx, cy, half, primitive->style.borderColor);
                    }
                    break;
                }
                case OverlayPrimitiveKind::Crosshair:
                {
                    const auto* desc = static_cast<const OverlayMarkerSetDesc*>(primitive->payload);
                    if (!desc || desc->count == 0)
                        break;
                    float cx = desc->positions[0];
                    float cy = desc->positions[1];
                    float len = primitive->style.pointSize;
                    float r, g, b, a;
                    unpackRGBA(primitive->style.borderColor, r, g, b, a);
                    count = 4;
                    m_unifiedVerts.reserve(start + count);
                    m_unifiedVerts.push_back(makeVert(cx - len, cy, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(cx + len, cy, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(cx, cy - len, z, r, g, b, a));
                    m_unifiedVerts.push_back(makeVert(cx, cy + len, z, r, g, b, a));
                    break;
                }
                case OverlayPrimitiveKind::SnapIndicator:
                {
                    const auto* desc = static_cast<const OverlayMarkerSetDesc*>(primitive->payload);
                    if (!desc || desc->count == 0)
                        break;
                    float cx = desc->positions[0];
                    float cy = desc->positions[1];
                    float radius = primitive->style.pointSize;
                    float r, g, b, a;
                    unpackRGBA(primitive->style.fillColor, r, g, b, a);
                    const int kSegments = 16;
                    count = kSegments * 2;
                    m_unifiedVerts.reserve(start + count);
                    for (int i = 0; i < kSegments; ++i)
                    {
                        float a0 = 2.0f * 3.14159265f * i / kSegments;
                        float a1 = 2.0f * 3.14159265f * (i + 1) / kSegments;
                        m_unifiedVerts.push_back(makeVert(cx + radius * std::cos(a0), cy + radius * std::sin(a0), z, r, g, b, a));
                        m_unifiedVerts.push_back(makeVert(cx + radius * std::cos(a1), cy + radius * std::sin(a1), z, r, g, b, a));
                    }
                    break;
                }
            }

            if (count > 0)
            {
                m_unifiedRanges.push_back(start);
                m_unifiedRanges.push_back(count);
                m_unifiedRanges.push_back(isTriangle);
                m_unifiedRanges.push_back(static_cast<uint32_t>(primitive->kind));
                m_dirty = true;
            }
        }

        void OverlayQueue::clearUnifiedOverlays()
        {
            m_unifiedVerts.clear();
            m_unifiedRanges.clear();
            m_dirty = true;
        }

        void OverlayQueue::clearOverlayKind(OverlayPrimitiveKind kind)
        {
            if (m_unifiedRanges.empty())
                return;

            std::vector<OverlayVertex> newVerts;
            std::vector<uint32_t> newRanges;
            newVerts.reserve(m_unifiedVerts.size());
            newRanges.reserve(m_unifiedRanges.size());

            uint32_t newOffset = 0;
            for (size_t i = 0; i < m_unifiedRanges.size(); i += 4)
            {
                uint32_t oldStart = m_unifiedRanges[i];
                uint32_t count = m_unifiedRanges[i + 1];
                uint32_t isTriangle = m_unifiedRanges[i + 2];
                OverlayPrimitiveKind rangeKind = static_cast<OverlayPrimitiveKind>(m_unifiedRanges[i + 3]);

                if (rangeKind == kind)
                    continue;

                newVerts.insert(newVerts.end(),
                    m_unifiedVerts.begin() + oldStart,
                    m_unifiedVerts.begin() + oldStart + count);

                newRanges.push_back(newOffset);
                newRanges.push_back(count);
                newRanges.push_back(isTriangle);
                newRanges.push_back(static_cast<uint32_t>(rangeKind));

                newOffset += count;
            }

            m_unifiedVerts = std::move(newVerts);
            m_unifiedRanges = std::move(newRanges);
            m_dirty = true;
        }

        void OverlayQueue::render(rhi::IDevice* device, CommandEncoder* encoder,
            const float viewMatrix[9])
        {
            (void)viewMatrix; // viewMatrix 由 CommandEncoder::execute() 统一设置

            if (!encoder)
            {
                SY_ERRORF("[OverlayQueue] render: encoder is null, cannot submit commands");
                return;
            }

            uint32_t totalOldVerts =
                static_cast<uint32_t>(m_selectionBoxVerts.size()) +
                static_cast<uint32_t>(m_controlVerts.size()) +
                static_cast<uint32_t>(m_previewVerts.size()) +
                static_cast<uint32_t>(m_markerVerts.size()) +
                static_cast<uint32_t>(m_handleVerts.size()) +
                static_cast<uint32_t>(m_crosshairVerts.size()) +
                static_cast<uint32_t>(m_snapVerts.size()) +
                static_cast<uint32_t>(m_selRectFillVerts.size()) +
                static_cast<uint32_t>(m_selRectBorderVerts.size());

            uint32_t totalUnifiedVerts = static_cast<uint32_t>(m_unifiedVerts.size());
            uint32_t totalVerts = totalOldVerts + totalUnifiedVerts;

            if (totalVerts == 0) return;

            // 仅在叠加层数据变化时重建合并顶点缓冲区和上传 GPU
            if (m_dirty)
            {
                if (totalVerts > m_vbCapacity)
                {
                    if (m_vertexBuffer != rhi::NullHandle)
                        device->destroyBuffer(m_vertexBuffer);

                    uint32_t newCap = m_vbCapacity;
                    if (newCap == 0) newCap = 4096;
                    while (newCap < totalVerts) newCap *= 2;

                    rhi::BufferDesc desc;
                    desc.size = newCap * sizeof(OverlayVertex);
                    desc.usage = rhi::BufferUsage::Vertex;
                    desc.memory = rhi::MemoryType::GPU_CPU_Coherent;
                    desc.debugName = "OverlayQueue_VB";
                    m_vertexBuffer = device->createBuffer(desc);
                    m_vbCapacity = newCap;
                }

                std::vector<OverlayVertex> merged;
                merged.reserve(totalVerts);

                uint32_t selStart = static_cast<uint32_t>(merged.size());
                uint32_t selCount = static_cast<uint32_t>(m_selectionBoxVerts.size());
                merged.insert(merged.end(), m_selectionBoxVerts.begin(), m_selectionBoxVerts.end());

                uint32_t ctrlStart = static_cast<uint32_t>(merged.size());
                uint32_t ctrlCount = static_cast<uint32_t>(m_controlVerts.size());
                merged.insert(merged.end(), m_controlVerts.begin(), m_controlVerts.end());

                uint32_t prevStart = static_cast<uint32_t>(merged.size());
                uint32_t prevCount = static_cast<uint32_t>(m_previewVerts.size());
                merged.insert(merged.end(), m_previewVerts.begin(), m_previewVerts.end());

                uint32_t markerStart = static_cast<uint32_t>(merged.size());
                uint32_t markerCount = static_cast<uint32_t>(m_markerVerts.size());
                merged.insert(merged.end(), m_markerVerts.begin(), m_markerVerts.end());

                uint32_t handleStart = static_cast<uint32_t>(merged.size());
                uint32_t handleCount = static_cast<uint32_t>(m_handleVerts.size());
                merged.insert(merged.end(), m_handleVerts.begin(), m_handleVerts.end());

                uint32_t crossStart = static_cast<uint32_t>(merged.size());
                uint32_t crossCount = static_cast<uint32_t>(m_crosshairVerts.size());
                merged.insert(merged.end(), m_crosshairVerts.begin(), m_crosshairVerts.end());

                uint32_t snapStart = static_cast<uint32_t>(merged.size());
                uint32_t snapCount = static_cast<uint32_t>(m_snapVerts.size());
                merged.insert(merged.end(), m_snapVerts.begin(), m_snapVerts.end());

                uint32_t rectFillStart = static_cast<uint32_t>(merged.size());
                uint32_t rectFillCount = static_cast<uint32_t>(m_selRectFillVerts.size());
                merged.insert(merged.end(), m_selRectFillVerts.begin(), m_selRectFillVerts.end());

                uint32_t rectBorderStart = static_cast<uint32_t>(merged.size());
                uint32_t rectBorderCount = static_cast<uint32_t>(m_selRectBorderVerts.size());
                merged.insert(merged.end(), m_selRectBorderVerts.begin(), m_selRectBorderVerts.end());

                // 追加统一提交的 overlay 顶点数据
                uint32_t unifiedStart = static_cast<uint32_t>(merged.size());
                merged.insert(merged.end(), m_unifiedVerts.begin(), m_unifiedVerts.end());

                device->uploadBuffer(m_vertexBuffer, 0,
                    merged.size() * sizeof(OverlayVertex),
                    merged.data());

                m_mergedOffsets[0] = selStart;     m_mergedCounts[0] = selCount;
                m_mergedOffsets[1] = ctrlStart;    m_mergedCounts[1] = ctrlCount;
                m_mergedOffsets[2] = prevStart;    m_mergedCounts[2] = prevCount;
                m_mergedOffsets[3] = markerStart;  m_mergedCounts[3] = markerCount;
                m_mergedOffsets[4] = handleStart;  m_mergedCounts[4] = handleCount;
                m_mergedOffsets[5] = crossStart;   m_mergedCounts[5] = crossCount;
                m_mergedOffsets[6] = snapStart;    m_mergedCounts[6] = snapCount;
                m_mergedOffsets[7] = rectFillStart;   m_mergedCounts[7] = rectFillCount;
                m_mergedOffsets[8] = rectBorderStart; m_mergedCounts[8] = rectBorderCount;

                m_unifiedStart = unifiedStart;
                m_dirty = false;
            }

            // Phase 3: 通过 CommandEncoder 提交绘制命令，不再直接调用 RHI
            uint32_t selCount = m_mergedCounts[0];
            uint32_t ctrlCount = m_mergedCounts[1];
            uint32_t prevCount = m_mergedCounts[2];
            uint32_t markerCount = m_mergedCounts[3];
            uint32_t handleCount = m_mergedCounts[4];
            uint32_t crossCount = m_mergedCounts[5];
            uint32_t snapCount = m_mergedCounts[6];
            uint32_t rectFillCount = m_mergedCounts[7];
            uint32_t rectBorderCount = m_mergedCounts[8];

            uint32_t selStart = m_mergedOffsets[0];
            uint32_t ctrlStart = m_mergedOffsets[1];
            uint32_t prevStart = m_mergedOffsets[2];
            uint32_t markerStart = m_mergedOffsets[3];
            uint32_t handleStart = m_mergedOffsets[4];
            uint32_t crossStart = m_mergedOffsets[5];
            uint32_t snapStart = m_mergedOffsets[6];
            uint32_t rectFillStart = m_mergedOffsets[7];
            uint32_t rectBorderStart = m_mergedOffsets[8];

            if (selCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, selStart, selCount);

            if (ctrlCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, ctrlStart, ctrlCount);

            if (prevCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, prevStart, prevCount);

            if (markerCount > 0)
            {
                uint32_t numMarkers = markerCount / m_markerVertsPerItem;
                uint32_t triCount = numMarkers * 6;
                uint32_t lineCount = numMarkers * 8;

                if (triCount > 0)
                    encoder->submitOverlay(PrimitiveType::TriangleList, markerStart, triCount);
                if (lineCount > 0)
                    encoder->submitOverlay(PrimitiveType::LineList,
                        markerStart + triCount, lineCount);
            }

            if (handleCount > 0)
            {
                uint32_t numHandles = handleCount / m_handleVertsPerItem;
                uint32_t triCount = numHandles * 6;
                uint32_t lineCount = numHandles * 8;

                if (triCount > 0)
                    encoder->submitOverlay(PrimitiveType::TriangleList, handleStart, triCount);
                if (lineCount > 0)
                    encoder->submitOverlay(PrimitiveType::LineList,
                        handleStart + triCount, lineCount);
            }

            if (crossCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, crossStart, crossCount);

            if (snapCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, snapStart, snapCount);

            if (rectFillCount > 0)
                encoder->submitOverlay(PrimitiveType::TriangleList, rectFillStart, rectFillCount);

            if (rectBorderCount > 0)
                encoder->submitOverlay(PrimitiveType::LineList, rectBorderStart, rectBorderCount);

            // 统一提交的 overlay 图元
            if (!m_unifiedRanges.empty() && totalUnifiedVerts > 0)
            {
                for (size_t i = 0; i < m_unifiedRanges.size(); i += 4)
                {
                    uint32_t start = m_unifiedRanges[i];
                    uint32_t count = m_unifiedRanges[i + 1];
                    uint32_t isTriangle = m_unifiedRanges[i + 2];

                    if (count == 0)
                        continue;

                    PrimitiveType topo = isTriangle
                        ? PrimitiveType::TriangleList
                        : PrimitiveType::LineList;
                    encoder->submitOverlay(topo, m_unifiedStart + start, count);
                }
            }
        }

        void OverlayQueue::buildMarkerQuad(OverlayVertex* out, float cx, float cy,
            float halfSize, uint32_t fillColor)
        {
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
            float halfSize, uint32_t borderColor)
        {
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
            uint32_t vertexCount, PrimitiveType type)
        {
            if (vertexCount == 0) return;

            device->uploadBuffer(m_vertexBuffer, 0,
                vertexCount * sizeof(OverlayVertex), data);

            if (type == PrimitiveType::TriangleList)
            {
                device->bindPipeline(m_trianglePipeline);
            }
            else
            {
                device->bindPipeline(m_linePipeline);
            }

            device->bindVertexBuffer(0, m_vertexBuffer, 0);
            device->draw(vertexCount, 1, 0, 0);
        }
    } // namespace core
} // namespace render