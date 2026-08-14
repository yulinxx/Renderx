/**
 * @file render_c_api_frame.cpp
 * @brief 渲染帧主循环、GPU剔除、场景模式、几何细分、统一几何提交
 *
 * 从 render_c_api.cpp 拆分而来，包含：
 * - GPU 剔除辅助函数（syncWorldToPersistentManager / computeViewBounds / readBackGpuVisibility）
 * - renderFrame 主渲染循环（2D/3D Pass 编排）
 * - 场景模式（renderBeginScene / renderEndScene）
 * - 几何细分函数（tessellatePolyline / tessellateCircle / tessellateArc / tessellateEllipse）
 * - 统一几何提交 API（renderSubmitGeometry / renderSubmitGeometries）
 *
 * 日志策略：生产环境仅输出 SY_DEBUGF 级别日志，
 * SY_INFOF 级别的冗余信息已移除以减少每帧性能开销。
 * 警告和错误仍通过 SY_WARNF/SY_ERRORF 输出。
 */
#include "render_c_api_internal.h"

#include <render/tess_params.h>

using namespace render;

// ============================================================================
// GPU 剔除辅助函数
// ============================================================================

/**
 * @brief 将 RenderWorld 图元同步到 PersistentEntityManager
 *
 * 每帧调用，全量同步当前所有图元元数据到 GPU SSBO。
 * 图元数量通常较小（几千到几万），全量同步的 CPU 开销可接受。
 */
static void syncWorldToPersistentManager(RenderDevice* dev)
{
    auto& pem = dev->persistentEntityManager;
    auto& world = dev->world2D;

    pem.clearEntities();

    const auto* entries = world.getEntityEntries();
    uint32_t count = world.getEntityCount();

    for (uint32_t i = 0; i < count; ++i)
    {
        const auto& e = entries[i];
        if (e.vertexCount == 0)
        {
            continue;
        }

        render::core::PersistentEntity pe{};
        pe.id = static_cast<uint32_t>(e.entityId);
        pe.bboxMin[0] = e.bbox[0];
        pe.bboxMin[1] = e.bbox[1];
        pe.bboxMin[2] = 0.0f;
        pe.bboxMax[0] = e.bbox[2];
        pe.bboxMax[1] = e.bbox[3];
        pe.bboxMax[2] = 0.0f;
        pe.worldPos[0] = (e.bbox[0] + e.bbox[2]) * 0.5f;
        pe.worldPos[1] = (e.bbox[1] + e.bbox[3]) * 0.5f;
        pe.worldPos[2] = 0.0f;
        pe.vertexOffset = e.vertexOffset;
        pe.vertexCount = e.vertexCount;
        pe.materialIndex = e.materialIndex;
        // bit0 = 1 表示可见（与 RenderWorld 的 kEntityFlagHidden 语义相反）
        pe.flags = (e.flags & 1u) ? 0u : 1u;

        // 传入 RenderWorld dense 数组索引 i，用于将 GPU 剔除结果（PEM 索引）
        // 正确转换回 RenderWorld dense 索引。不能用 entityId，二者不等价。
        pem.addEntity(pe, i);
    }
}

/**
 * @brief 从 viewMatrix 计算 2D 视图矩形（世界空间）
 *
 * 复用 CPU 四叉树 queryVisible 的逆矩阵逻辑，将 NDC 角点 [-1,-1]~[1,1]
 * 变换回世界空间，得到与四叉树完全一致的视图矩形。
 *
 * @param viewMatrix 3x3 视图矩阵（列主序）
 * @param outMinX 输出最小 X
 * @param outMinY 输出最小 Y
 * @param outMaxX 输出最大 X
 * @param outMaxY 输出最大 Y
 */
static void computeViewBounds(const float viewMatrix[9], float* outMinX, float* outMinY, float* outMaxX, float* outMaxY)
{
    // Camera2D 生成的 viewMatrix 是 column-major (列主序) 3x3:
    // 内存布局 (按列存储):
    // col 0: [ scaleX,     0,       0 ]  -> data[0], data[1], data[2]
    // col 1: [ 0,       scaleY,     0 ]  -> data[3], data[4], data[5]
    // col 2: [ tx,       ty,       1 ]  -> data[6], data[7], data[8]
    //
    // 矩阵形式:
    // [ scaleX   0       tx ]
    // [ 0       scaleY   ty ]
    // [ 0       0       1  ]
    //
    // 这是 world -> NDC 变换。我们需要 inverse (NDC -> world):
    // [ 1/scaleX   0       -tx/scaleX ]
    // [ 0       1/scaleY   -ty/scaleY ]
    // [ 0       0           1       ]

    float scaleX = viewMatrix[0];
    float scaleY = viewMatrix[4];
    float tx = viewMatrix[6];
    float ty = viewMatrix[7];

    if (std::abs(scaleX) < 1e-10f || std::abs(scaleY) < 1e-10f)
    {
        *outMinX = *outMinY = -FLT_MAX;
        *outMaxX = *outMaxY = FLT_MAX;
        SY_WARNF("computeViewBounds: invalid scale (sx=%.6f, sy=%.6f), returning infinite bounds", scaleX, scaleY);
        return;
    }

    float invScaleX = 1.0f / scaleX;
    float invScaleY = 1.0f / scaleY;
    float invTx = -tx * invScaleX;
    float invTy = -ty * invScaleY;

    // NDC 4 个角点 (-1,-1), (1,-1), (1,1), (-1,1) -> world
    // 列主序逆矩阵乘法: [x,y,1] * inv = [ x*inv[0]+y*inv[3]+inv[6], x*inv[1]+y*inv[4]+inv[7], ... ]
    static const float kCorners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };

    float minX = FLT_MAX, minY = FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX;
    float cornerWorldX[4], cornerWorldY[4];
    int validCorners = 0;

    for (int i = 0; i < 4; ++i)
    {
        float ndcX = kCorners[i][0];
        float ndcY = kCorners[i][1];

        // world = [ndcX, ndcY, 1] * inv_matrix (column-major)
        float wx = ndcX * invScaleX + invTx;
        float wy = ndcY * invScaleY + invTy;

        cornerWorldX[i] = wx;
        cornerWorldY[i] = wy;
        minX = (std::min)(minX, wx);
        minY = (std::min)(minY, wy);
        maxX = (std::max)(maxX, wx);
        maxY = (std::max)(maxY, wy);
        validCorners++;
    }

    *outMinX = minX;
    *outMinY = minY;
    *outMaxX = maxX;
    *outMaxY = maxY;

    // 调试日志
    static int cullDebugCounter = 0;
    if (cullDebugCounter < 1)
    {
        SY_DEBUGF("computeViewBounds: scaleX=%.6f scaleY=%.6f tx=%.2f ty=%.2f, bounds=[%.2f,%.2f]-[%.2f,%.2f], "
                  "validCorners=%d",
            scaleX,
            scaleY,
            tx,
            ty,
            minX,
            minY,
            maxX,
            maxY,
            validCorners);
        for (int i = 0; i < validCorners && i < 4; ++i)
        {
            SY_DEBUGF("  corner %d (%.1f,%.1f) -> world (%.2f,%.2f)",
                i,
                kCorners[i][0],
                kCorners[i][1],
                cornerWorldX[i],
                cornerWorldY[i]);
        }
        cullDebugCounter++;
    }
}

/**
 * @brief 回读 GPU 剔除的可见性结果
 *
 * 从 PersistentEntityManager 的 visibility buffer 回读，生成可见图元索引数组。
 * 回读会阻塞 CPU，作为基础实现可接受。后续可改为异步查询或 GPU-driven 链路。
 *
 * @return 可见图元数量
 */
/**
 * @brief 从 GPU 可见性缓冲区回读可见图元索引（M8: 异步优化版）
 *
 * 使用 PersistentEntityManager 的异步读取接口，
 * 避免直接 mapBuffer 阻塞 CPU。
 *
 * @param dev 渲染设备
 * @param outIndices 输出可见图元索引数组
 * @param maxOut 输出缓冲区容量
 * @return 可见图元数量
 */
static uint32_t readBackGpuVisibility(RenderDevice* dev, uint32_t* outIndices, uint32_t maxOut)
{
    auto& pem = dev->persistentEntityManager;
    // 同步回读当前帧 GPU 剔除结果（阻塞 mapBuffer 一次）。
    // M8 曾设计"跨帧双缓冲异步回读"，但 clearEntities() 每帧全量重建 PEM，
    // 缓存无法跨帧复用，已收敛为统一的同步回读，保证正确性优先。
    return pem.readBackGpuVisibility(outIndices, maxOut);
}

// ============================================================================
// 几何细分函数
// ============================================================================

/**
 * @brief 将多段线几何数据细分为顶点
 *
 * 使用 double 精度减去相机中心后再转 float，消除大坐标的精度损失。
 *
 * @param polyline 多段线几何数据
 * @param outVertices 输出顶点数组
 */
static void tessellatePolyline(const GeometryPolyline* polyline, std::vector<render::VertexP3C3>& outVertices)
{
    if (!polyline || !polyline->points || polyline->pointCount < 2)
    {
        return;
    }

    float cr = polyline->color[0], cg = polyline->color[1], cb = polyline->color[2];
    outVertices.reserve(polyline->pointCount);
    for (uint32_t i = 0; i < polyline->pointCount; ++i)
    {
        outVertices.push_back(
            { static_cast<float>(polyline->points[i].x), static_cast<float>(polyline->points[i].y), 0.0f, cr, cg, cb });
    }
    // 闭合折线由调用方以 LineLoop 提交，首尾自动闭合，此处不再追加首点副本，
    // 与增量路径（EntityToVertices::IncrementalVertexSink）保持顶点数/拓扑一致，
    // 避免 modifyEntity 保留旧拓扑时出现“缺边/一段显示一段不显示”。
}

/**
 * @brief 将圆形几何数据细分为顶点
 *
 * 使用64段线段近似圆形。以 double 精度减去相机中心。
 *
 * @param circle 圆形几何数据
 * @param outVertices 输出顶点数组
 */
static void tessellateCircle(const GeometryCircle* circle, std::vector<render::VertexP3C3>& outVertices)
{
    if (!circle || circle->radius <= 0)
    {
        return;
    }

    float cr = circle->color[0], cg = circle->color[1], cb = circle->color[2];
    const int segments = render::tess::kCircleSegments;
    outVertices.reserve(segments);
    const double centerX = circle->center.x;
    const double centerY = circle->center.y;
    const double radius = circle->radius;

    for (int i = 0; i < segments; ++i)
    {
        double angle = (2.0 * render::tess::kPi * i) / segments;
        outVertices.push_back({ static_cast<float>(centerX + radius * std::cos(angle)),
            static_cast<float>(centerY + radius * std::sin(angle)),
            0.0f,
            cr,
            cg,
            cb });
    }
}

/**
 * @brief 将圆弧几何数据细分为顶点
 *
 * 根据圆弧角度范围动态计算细分段数（8-128段）。以 double 精度减去相机中心。
 *
 * @param arc 圆弧几何数据
 * @param outVertices 输出顶点数组
 */
static void tessellateArc(const GeometryArc* arc, std::vector<render::VertexP3C3>& outVertices)
{
    if (!arc || arc->radius <= 0)
    {
        return;
    }

    float cr = arc->color[0], cg = arc->color[1], cb = arc->color[2];
    double start = arc->startAngle;
    double end = arc->endAngle;
    if (end < start)
    {
        end += 2.0 * render::tess::kPi;
    }

    double angleRange = end - start;
    int segments = render::tess::arcSegments(angleRange);

    outVertices.reserve(segments);
    const double centerX = arc->center.x;
    const double centerY = arc->center.y;
    const double radius = arc->radius;

    for (int i = 0; i <= segments; ++i)
    {
        double t = static_cast<double>(i) / segments;
        double angle = start + t * angleRange;
        outVertices.push_back({ static_cast<float>(centerX + radius * std::cos(angle)),
            static_cast<float>(centerY + radius * std::sin(angle)),
            0.0f,
            cr,
            cg,
            cb });
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
    {
        return;
    }

    // 统一离散化基准段数：64（完整椭圆）
    const int segments = render::tess::kCircleSegments;
    outVertices.reserve(segments);

    double start = ellipse->startAngle;
    double end = ellipse->endAngle;
    if (ellipse->fullEllipse || (start == 0.0 && end == 0.0))
    {
        start = 0.0;
        end = 2.0 * render::tess::kPi;
    }
    // 椭圆弧角度归一化：end < start 时跨 2π，与增量路径一致
    if (end < start)
    {
        end += 2.0 * render::tess::kPi;
    }

    double angleRange = end - start;
    // 统一离散化段数：完整椭圆 64，椭圆弧按角度比例缩放
    int actualSegments = render::tess::ellipseSegments(angleRange);

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

        outVertices.push_back({ static_cast<float>(centerX + x * cosRot - y * sinRot),
            static_cast<float>(centerY + x * sinRot + y * cosRot),
            0.0f,
            cr,
            cg,
            cb });
    }
}

// ============================================================================
// 统一几何提交 API
// ============================================================================

/**
 * @brief 解析图元ID：优先使用外部传入的ID，为0时回退到设备实例级计数器
 */
inline EntityId resolveEntityId(RenderDevice* dev, EntityId explicitId)
{
    return (explicitId != 0) ? explicitId : dev->entityIdCounter++;
}

/**
 * @brief 提交单个几何图元（统一 API）
 *
 * 根据 GeometryPrimitive::kind 分发到对应渲染路径：
 * - Polyline / Circle / Arc / Ellipse / Image → tessellate 后 world2D.addEntity
 * - Text → 暂存到 pendingTextItems，renderFrame 时由 TextAtlas 渲染
 * - TriangleSoup → meshManager.registerMesh + addInstance
 */
static void renderSubmitGeometryImpl(RenderDevice* dev, const GeometryPrimitive* primitive)
{
    if (!dev || !primitive)
    {
        return;
    }

    switch (primitive->kind)
    {
    // ---- 2D 文档几何路径 ----
    case GeometryPrimitiveKind::Polyline:
    {
        if (!primitive->desc.polyline)
        {
            return;
        }
        std::vector<render::VertexP3C3> vertices;
        tessellatePolyline(primitive->desc.polyline, vertices);
        if (!vertices.empty())
        {
            render::PrimitiveType type = primitive->desc.polyline->closed ? render::PrimitiveType::LineLoop
                                                                          : render::PrimitiveType::LineStrip;
            EntityId eid = resolveEntityId(dev, primitive->entityId);
            dev->world2D.addEntity(eid, vertices.data(), static_cast<uint32_t>(vertices.size()), type, 0);
        }
        break;
    }
    case GeometryPrimitiveKind::Circle:
    {
        if (!primitive->desc.circle)
        {
            return;
        }
        std::vector<render::VertexP3C3> vertices;
        tessellateCircle(primitive->desc.circle, vertices);
        if (!vertices.empty())
        {
            EntityId eid = resolveEntityId(dev, primitive->entityId);
            dev->world2D.addEntity(
                eid, vertices.data(), static_cast<uint32_t>(vertices.size()), render::PrimitiveType::LineLoop, 0);
        }
        break;
    }
    case GeometryPrimitiveKind::Arc:
    {
        if (!primitive->desc.arc)
        {
            return;
        }
        std::vector<render::VertexP3C3> vertices;
        tessellateArc(primitive->desc.arc, vertices);
        if (!vertices.empty())
        {
            EntityId eid = resolveEntityId(dev, primitive->entityId);
            dev->world2D.addEntity(
                eid, vertices.data(), static_cast<uint32_t>(vertices.size()), render::PrimitiveType::LineStrip, 0);
        }
        break;
    }
    case GeometryPrimitiveKind::Ellipse:
    {
        if (!primitive->desc.ellipse)
        {
            return;
        }
        std::vector<render::VertexP3C3> vertices;
        tessellateEllipse(primitive->desc.ellipse, vertices);
        if (!vertices.empty())
        {
            render::PrimitiveType type = primitive->desc.ellipse->fullEllipse ? render::PrimitiveType::LineLoop
                                                                              : render::PrimitiveType::LineStrip;
            EntityId eid = resolveEntityId(dev, primitive->entityId);
            dev->world2D.addEntity(eid, vertices.data(), static_cast<uint32_t>(vertices.size()), type, 0);
        }
        break;
    }
    case GeometryPrimitiveKind::Image:
    {
        if (!primitive->desc.image)
        {
            return;
        }
        const GeometryImage* image = primitive->desc.image;
        std::vector<render::VertexP3C3> vertices;
        vertices.reserve(5);
        render::VertexP3C3 v;
        v.cr = image->color[0];
        v.cg = image->color[1];
        v.cb = image->color[2];
        v.px = static_cast<float>(image->topLeft.x);
        v.py = static_cast<float>(image->topLeft.y);
        v.pz = 0.0f;
        vertices.push_back(v);
        v.px = static_cast<float>(image->topRight.x);
        v.py = static_cast<float>(image->topRight.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->bottomRight.x);
        v.py = static_cast<float>(image->bottomRight.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->bottomLeft.x);
        v.py = static_cast<float>(image->bottomLeft.y);
        vertices.push_back(v);
        v.px = static_cast<float>(image->topLeft.x);
        v.py = static_cast<float>(image->topLeft.y);
        vertices.push_back(v);
        EntityId eid = resolveEntityId(dev, primitive->entityId);
        dev->world2D.addEntity(
            eid, vertices.data(), static_cast<uint32_t>(vertices.size()), render::PrimitiveType::LineStrip, 0);
        break;
    }
    // ---- 文本缓存路径 ----
    case GeometryPrimitiveKind::Text:
    {
        if (!primitive->desc.text || !primitive->desc.text->text)
        {
            return;
        }
        const GeometryText* text = primitive->desc.text;
        RenderDevice::PendingText pt;
        pt.textStorage = text->text;
        pt.item.text = pt.textStorage.c_str();
        pt.item.x = static_cast<float>(text->position.x);
        pt.item.y = static_cast<float>(text->position.y);
        pt.item.coordMode = 0;
        pt.item.hAlign = text->hAlign;
        pt.item.vAlign = text->vAlign;
        pt.item.fontSize = (text->fontSize > 0.0f) ? static_cast<int32_t>(text->fontSize) : 12;
        pt.item.color[0] = text->color[0];
        pt.item.color[1] = text->color[1];
        pt.item.color[2] = text->color[2];
        pt.item.color[3] = text->color[3];
        pt.item.rotationDeg = text->rotationDeg;
        pt.item.zOrder = 0.0f;
        dev->pendingTextItems.push_back(std::move(pt));
        break;
    }
    // ---- 3D mesh 路径 ----
    case GeometryPrimitiveKind::TriangleSoup:
    {
        if (!primitive->desc.triangleSoup)
        {
            return;
        }
        const GeometryTriangleSoupDesc* desc = primitive->desc.triangleSoup;
        if (!desc->vertices || !desc->normals || desc->vertexCount < 3)
        {
            return;
        }

        // 生成顺序索引
        std::vector<uint32_t> indices(desc->vertexCount);
        for (uint32_t i = 0; i < desc->vertexCount; ++i)
        {
            indices[i] = i;
        }

        MeshId meshId = dev->meshManager.registerMesh(
            desc->vertices, desc->normals, indices.data(), desc->vertexCount, desc->vertexCount);
        if (meshId == INVALID_MESH_ID)
        {
            return;
        }

        // 默认单位矩阵作为模型变换
        float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        dev->meshManager.addInstance(meshId, identity, 0, const_cast<float*>(desc->color));
        break;
    }
    }
}

extern "C"
{
    // ==================== 渲染帧主循环 ====================

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
            uint32_t maxVisible = static_cast<uint32_t>(dev->world2D.getEntityCount());
            if (dev->visibleIndices.size() < maxVisible)
            {
                dev->visibleIndices.resize(maxVisible);
            }

            // ---- GPU 剔除（PersistentEntityManager）----
            // 1. 同步 RenderWorld 图元到 PersistentEntityManager
            // 仅在 RenderWorld 变更（addEntity/modifyEntity/removeEntity）时重建 PEM，
            // pan/zoom 不修改 RenderWorld，可跳过每帧全量重建，避免大规模图元时的卡顿
            const uint32_t worldGen = dev->world2D.getGeneration();
            if (worldGen != dev->lastWorld2DGeneration)
            {
                syncWorldToPersistentManager(dev);
                dev->persistentEntityManager.uploadChanges();
                dev->lastWorld2DGeneration = worldGen;
            }

            // 2. 计算 2D 视图矩形（与 CPU 四叉树一致的语义）
            float viewMinX, viewMinY, viewMaxX, viewMaxY;
            computeViewBounds(dev->view2D.viewMatrix, &viewMinX, &viewMinY, &viewMaxX, &viewMaxY);

            // 3. 执行 GPU 视锥剔除（2D AABB-矩形测试）
            dev->persistentEntityManager.executeCulling(viewMinX, viewMinY, viewMaxX, viewMaxY);

            // 4. 回读 GPU 剔除的可见性结果
            //    全量回读 visibility buffer（阻塞 mapBuffer 一次），同时收集
            //    可见 PEM 索引到 m_visiblePemIndices。这是唯一的回读入口，
            //    不再额外调用 generateIndirectCommands（那会再 map 一次 countBuffer，
            //    造成每帧两次阻塞 map，且其 indirect buffer 未被实际绘制路径使用）。
            uint32_t gpuVisibleCount = readBackGpuVisibility(
                dev, dev->visibleIndices.data(), static_cast<uint32_t>(dev->visibleIndices.size()));

            // 5. GPU 剔除成功则使用其结果，否则回退到 CPU 四叉树
            if (gpuVisibleCount > 0)
            {
                visibleCount = gpuVisibleCount;
            }
            else
            {
                // GPU 剔除返回 0：可能是视图内确实无图元（用户缩小/平移出界），
                // 也可能是剔除链路异常。用 CPU 四叉树交叉验证，避免误判。
                dev->world2D.queryVisible(dev->view2D.viewMatrix,
                    dev->view2D.viewWidth,
                    dev->view2D.viewHeight,
                    dev->visibleIndices.data(),
                    &visibleCount,
                    maxVisible);

                if (maxVisible > 0 && visibleCount == 0)
                {
                    // CPU 四叉树也返回 0。判断 viewMatrix 是否退化（scale 无效）：
                    // - 退化矩阵（scale≈0）：剔除结果不可信，强制显示全部，宁可多画不丢图元。
                    // - 有效矩阵：视图内确实无图元可见，保持 0（不强制显示，否则缩放到空白处会误画全部）。
                    const float sx = dev->view2D.viewMatrix[0];
                    const float sy = dev->view2D.viewMatrix[4];
                    const bool degenerate = std::abs(sx) < 1e-10f || std::abs(sy) < 1e-10f;
                    if (degenerate)
                    {
                        SY_WARNF("renderFrame: degenerate viewMatrix (sx=%.4f sy=%.4f), forcing all entities", sx, sy);
                        visibleCount = maxVisible;
                        for (uint32_t i = 0; i < maxVisible; ++i)
                        {
                            dev->visibleIndices[i] = i;
                        }
                    }
                    else
                    {
                        // 视图内无可见图元：保持 0，不强制显示
                    }
                }
            }

            // 视图参数摘要日志已移除，避免每帧输出

            // 先提交给 BatchQueue（此时 dirty 标志尚未清除，增量更新可正常工作）
            dev->batchQueue.submit(dev->visibleIndices.data(), visibleCount, dev->world2D);

            // 提交完成后清除 dirty 标志（不影响 BatchQueue 的增量顶点上传）
            dev->world2D.clearDirtyFlags();

            // ---- Pass 0: FrameSetup ----
            // 设置清屏颜色、深度测试、混合状态，并重置命令编码器
            {
                core::PassDesc pass;
                pass.name = "FrameSetup";
                pass.enabled = true;
                pass.onSetup = [dev](rhi::IDevice* d) {
                    d->setClearColor(dev->clearColor[0], dev->clearColor[1], dev->clearColor[2], dev->clearColor[3]);
                    d->enableDepthTest(false);
                    d->enableBlend(true);
                    dev->commandEncoder.reset();
                };
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 1: SceneEnv ----
            // 渲染场景环境（网格背景等）
            {
                core::PassDesc pass;
                pass.name = "SceneEnv";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->sceneEnv.render(d,
                        dev->view2D.viewMatrix,
                        static_cast<uint32_t>(dev->view2D.viewWidth),
                        static_cast<uint32_t>(dev->view2D.viewHeight));
                };
                pass.inputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Read, "Backbuffer", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 2: Bitmap ----
            // 渲染位图（纹理四边形），位于网格/台面之上、矢量几何与选择框之下
            {
                core::PassDesc pass;
                pass.name = "Bitmap";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    if (!dev->bitmapRenderer.hasBitmap())
                    {
                        return;
                    }
                    dev->bitmapRenderer.render(d, dev->view2D.viewMatrix, dev->cameraCenter);
                };
                pass.inputs.push_back(
                    { core::PassResourceType::Texture, core::PassResourceAccess::Read, "Bitmap_Tex", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "Bitmap_VB", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 3: World2DCollect ----
            // 将 world2D 图元渲染命令收集到 CommandEncoder
            {
                core::PassDesc pass;
                pass.name = "World2DCollect";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->batchQueue.render(d, &dev->commandEncoder, dev->view2D.viewMatrix, dev->world2D);
                };
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "World2D_VB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::IndirectBuffer, core::PassResourceAccess::Read, "BatchQueue_IB", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 4: OverlayCollect ----
            // 将 overlay 渲染命令收集到 CommandEncoder
            {
                core::PassDesc pass;
                pass.name = "OverlayCollect";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    dev->overlayQueue.render(d, &dev->commandEncoder, dev->view2D.viewMatrix);
                };
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "OverlayQueue_VB", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 5: CommandExecute ----
            // 统一执行所有已收集的绘制命令（World2D + Overlay）
            {
                core::PassDesc pass;
                pass.name = "CommandExecute";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    const float camCenterF[2] = { static_cast<float>(dev->cameraCenter[0]),
                        static_cast<float>(dev->cameraCenter[1]) };
                    dev->commandEncoder.execute(d,
                        dev->batchQueue.getVertexBuffer(),
                        dev->overlayQueue.getVertexBuffer(),
                        dev->batchQueue.getIndirectBuffer(),
                        dev->view2D.viewMatrix,
                        camCenterF);
                };
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "BatchQueue_VB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "OverlayQueue_VB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::IndirectBuffer, core::PassResourceAccess::Read, "BatchQueue_IB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::UniformBuffer, core::PassResourceAccess::Read, "ViewMatrix_UB", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // ---- Pass 6: Text ----
            // 渲染文本（在 overlay 之上）
            if (!dev->pendingTextItems.empty())
            {
                core::PassDesc pass;
                pass.name = "Text";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    if (dev->pendingTextItems.empty())
                    {
                        return;
                    }
                    std::vector<TextItem> items;
                    items.reserve(dev->pendingTextItems.size());
                    for (auto& pt : dev->pendingTextItems)
                    {
                        items.push_back(pt.item);
                    }
                    TextItemList list;
                    list.items = items.data();
                    list.count = static_cast<uint32_t>(items.size());
                    dev->textAtlas.renderText(&list, dev->view2D.viewMatrix, dev->viewportWidth, dev->viewportHeight, d);
                    dev->pendingTextItems.clear();
                };
                pass.inputs.push_back(
                    { core::PassResourceType::Texture, core::PassResourceAccess::Read, "TextAtlas_Tex", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "Text_VB", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
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
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::DepthTarget, core::PassResourceAccess::Write, "DepthBuffer", 0 });
                dev->renderGraph.addPass(pass);
            }

            // Pass 1: Mesh3D
            if (dev->meshManager.getInstanceCount() > 0)
            {
                core::PassDesc pass;
                pass.name = "Mesh3D";
                pass.enabled = true;
                pass.onExecute = [dev](rhi::IDevice* d) {
                    if (dev->meshManager.getInstanceCount() == 0)
                    {
                        return;
                    }
                    dev->meshManager.update();
                    dev->meshManager.render(d, dev->view3D.viewMatrix, dev->view3D.projMatrix);
                };
                pass.inputs.push_back(
                    { core::PassResourceType::VertexBuffer, core::PassResourceAccess::Read, "MeshManager_VB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::IndexBuffer, core::PassResourceAccess::Read, "MeshManager_IB", 0 });
                pass.inputs.push_back(
                    { core::PassResourceType::UniformBuffer, core::PassResourceAccess::Read, "ViewProj_UB", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::ColorTarget, core::PassResourceAccess::Write, "Backbuffer", 0 });
                pass.outputs.push_back(
                    { core::PassResourceType::DepthTarget, core::PassResourceAccess::Write, "DepthBuffer", 0 });
                dev->renderGraph.addPass(pass);
            }
        }

        // M5: 在执行前检查 Pass 之间资源冲突
        dev->renderGraph.checkResourceConflicts();

        // Phase 4: 按 Pass 顺序统一执行
        dev->renderGraph.execute(rhi);

        // 屏幕文本渲染（在所有场景内容之后）
        if (!dev->pendingScreenTexts.empty())
        {
            dev->screenTextRenderer.beginFrame();
            for (const auto& pst : dev->pendingScreenTexts)
            {
                ScreenTextItem item;
                item.text = pst.text.c_str();
                item.x = pst.x;
                item.y = pst.y;
                item.color[0] = pst.color[0];
                item.color[1] = pst.color[1];
                item.color[2] = pst.color[2];
                item.color[3] = pst.color[3];
                item.fontSize = pst.fontSize;
                dev->screenTextRenderer.submitText(item);
            }
            dev->screenTextRenderer.render(rhi, dev->view2D.viewWidth, dev->view2D.viewHeight);
            dev->pendingScreenTexts.clear();
        }

        rhi->endFrame();
        rhi->present();

        dev->stats.entityCount = dev->world2D.getEntityCount();
        dev->stats.visibleCount = visibleCount;
        dev->stats.gpuMemoryBytes = rhi->getGPUMemoryUsage();

        ++dev->frameCounter;
        // RenderStats 摘要日志已移除：每帧输出导致缩放卡顿
        // 调试时可通过 renderGetStats() 主动查询
        if (dev->frameCounter >= 60)
        {
            dev->frameCounter = 0;
        }
    }

    // ==================== 场景模式 ====================

    /**
     * @brief 开始场景模式
     *
     * 清除所有旧图元，重置图元ID计数器，准备接收新的场景数据。
     *
     * @param dev 渲染设备指针
     *
     * @note renderBeginScene() 是"场景重建入口"，不是每帧都应该调用。
     *       高频交互应使用 renderSubmitGeometry() / renderAddEntity() 等增量接口。
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
        dev->bitmapRenderer.clear();
        dev->entityIdCounter = 1;
    }

    /**
     * @brief 结束场景模式（预留接口）
     *
     * @param dev 渲染设备指针
     */
    RENDER_API void renderEndScene(RenderDevice* dev)
    {
        if (!dev)
        {
            return;
        }
        // 末态实体数日志已移除：仅在异常路径用 SY_ERROR 输出
    }

    // ==================== 统一几何提交 API ====================

    /**
     * @brief 提交单个几何图元（统一 API，公开入口）
     *
     * @note renderSubmitGeometry() 是"场景编译提交入口"，不是所有 overlay 的入口。
     *       overlay / text / mesh / env 应使用各自的独立提交接口。
     */
    RENDER_API void renderSubmitGeometry(RenderDevice* dev, const GeometryPrimitive* primitive)
    {
        renderSubmitGeometryImpl(dev, primitive);
    }

    /**
     * @brief 批量提交几何图元（统一 API）
     */
    RENDER_API void renderSubmitGeometries(RenderDevice* dev, const GeometryPrimitive* primitives, uint32_t count)
    {
        if (!dev || !primitives || count == 0)
        {
            return;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            renderSubmitGeometryImpl(dev, &primitives[i]);
        }
    }

    // ==================== Camera Center ====================

    RENDER_API void renderSetCameraCenter(RenderDevice* dev, double cx, double cy)
    {
        if (!dev)
        {
            return;
        }
        dev->cameraCenter[0] = cx;
        dev->cameraCenter[1] = cy;
    }
}  // extern "C"