/**
 * @file RenderTypes.h
 * @brief legacy 公共类型头（收缩中，将在 Phase 6 随 runtime_session.h 一并删除）
 *
 * ==================== 状态说明 ====================
 *
 * 本文件已从 929 行削减至当前规模，仅保留旧 RT C API（runtime_session.h）
 * 签名仍在引用的类型。新的公共 ABI 契约见 include/render/renderx.h，
 * 那是最终形态的唯一公共头。
 *
 * 之所以不能立刻删除：runtime_session.h 与 renderx.h 在 namespace Render::RT
 * 中定义了同名类型（Backend / RenderSpace / DefaultPipeline / BufferHandle /
 * PipelineHandle / TextureHandle），两者无法在同一编译单元共存。待 Phase 6
 * 的 Runtime/Surface/Session 实现切到 renderx.h 之后，本文件与
 * runtime_session.h 一并删除。
 *
 * ==================== 已删除的内容与原因 ====================
 *
 * 1. 全部含 std::vector 成员的结构（RenderCommand / RenderCommandList /
 *    RenderFrame / RenderFrameUpdate / RenderOverlayUpdate / RenderBitmapQuad /
 *    SelectionOutlinePath）以及返回 std::vector 的 inline 模板
 *    （toVec2fVector）、模板类 RenderVec<T,N>。
 *    这些与真正的 ABI 类型混在同一个公共头里且无任何隔离标注。虽然当时
 *    没有一个导出函数用到它们，但只要后续有人把它们放进导出签名，就会
 *    立刻引入 allocator / _ITERATOR_DEBUG_LEVEL / MSVC Debug-vs-Release
 *    不匹配的堆崩溃。
 *
 * 2. CAD 编辑器交互词汇：OverlayGroup{Preview, Control, SelectionBox,
 *    SelectionOutlines, SelectionHandles, PointMarkers, Snap}、
 *    OverlayForm::SnapCircle、OverlayData{crosshairWorld, snapWorld,
 *    mouseWorld}（鼠标状态放在渲染结构里）、SelectionDashStyle 的
 *    marching-ants 动画参数、EntityFlags::Selected。
 *    这些是编辑器交互状态，不是图形概念；且在 src/ 中零引用——实际 API
 *    早已改用不透明的 uint32_t group。
 *
 * 3. 文档几何词汇：GeometryPrimitiveKind{Polyline, Circle, Arc, Ellipse,
 *    Image, Text, TriangleSoup} 及 GeometryPolyline / GeometryCircle /
 *    GeometryArc / GeometryEllipse。
 *    渲染 DLL 的输入字母表应当只有顶点 + 拓扑。接受解析曲线迫使 DLL 承担
 *    离散化决策，这是同一套公式在三处重复实现的根源。
 *
 * 4. DeviceDesc / EntityDesc / EntityUpdate / ViewDesc2D / ViewDesc3D /
 *    OverlayDrawRange / RenderStats / EnvLayerDesc / SceneEnvGeometryDesc /
 *    RenderTargetHandle / RenderTargetDesc / DepthFormat / PrimitiveType /
 *    VertexP3C3 / VertexP3N3 / OverlayVertex / TextItem / TextItemList /
 *    BBox2f / Vec2d 等：随 legacy C API 与其实现一并失效。
 *    其中 RenderTarget* 已下移到 src/rhi/rhiTypes.h（纯 RHI 概念），
 *    TextItem* 已下移到 src/core/textAtlas.h（文本布局归应用层，
 *    DLL 只留字形图集）。
 */
#pragma once

#include <cstdint>

namespace Render
{

    /**
     * @brief 材质描述
     *
     * 纯 POD。注意 ABI 面不使用 bool（C++ 未规定 sizeof(bool)）。
     */
    struct MaterialDesc
    {
        float lineWidth;  ///< 线宽（像素）。各后端上限不同，macOS 上通常只能是 1.0
        float pointSize;  ///< 点大小（像素）
        float color[4];   ///< RGBA，范围 [0,1]
        uint32_t flags;   ///< 材质标志位
    };

    static_assert(sizeof(MaterialDesc) == 28, "MaterialDesc ABI size changed");

}  // namespace Render
