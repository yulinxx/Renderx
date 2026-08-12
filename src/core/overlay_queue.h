/**
 * @file overlay_queue.h
 * @brief 叠加层渲染队列类定义
 * 
 * OverlayQueue 负责渲染叠加在场景之上的UI元素，包括：
 * - 十字准星（Crosshair）
 * - 捕捉指示器（Snap Indicator）
 * - 预览线（Preview Lines）
 * - 控制线（Control Lines）
 * - 点标记（Point Markers）
 * - 选择框（Selection Box）
 * - 选择手柄（Selection Handles）
 * 
 * 所有叠加元素使用世界坐标，通过视图矩阵转换到屏幕空间。
 */
#pragma once

#include "render/render_types.h"
#include "../rhi/rhi_device.h"
#include <vector>
#include <cstdint>

namespace render {
namespace core {

// Phase 3: 前向声明统一命令编码器
class CommandEncoder;

/**
 * @brief 叠加层渲染队列类
 * 
 * 管理各种叠加元素的顶点数据，批量上传并渲染。
 */
class OverlayQueue {
public:
    /**
     * @brief 初始化叠加层渲染器
     *
     * @param device RHI设备指针
     * @return true 初始化成功，false 初始化失败
     */
    bool initialize(rhi::IDevice* device);

    /**
     * @brief 关闭并释放所有资源
     */
    void shutdown();

    /**
     * @brief 设置十字准星
     * 
     * @param worldX 世界坐标X
     * @param worldY 世界坐标Y
     * @param visible 是否可见
     */
    void setCrosshair(float worldX, float worldY, bool visible);

    /**
     * @brief 设置捕捉指示器
     * 
     * @param worldX 世界坐标X
     * @param worldY 世界坐标Y
     * @param visible 是否可见
     * @param color 颜色（RGBA格式）
     */
    void setSnapIndicator(float worldX, float worldY, bool visible,
                          const float color[4]);

    /**
     * @brief 设置预览线
     * 
     * @param vertices 顶点数据
     * @param count 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    void setPreviewLines(const VertexP3C3* vertices, uint32_t count,
                         uint32_t colorRGBA);

    /**
     * @brief 设置控制线
     * 
     * @param vertices 顶点数据
     * @param count 顶点数量
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    void setControlLines(const VertexP3C3* vertices, uint32_t count,
                         uint32_t colorRGBA);

    /**
     * @brief 设置点标记
     * 
     * @param worldPositions 世界坐标数组（每点2个float）
     * @param count 点数量
     * @param markerSize 标记大小（像素）
     * @param fillColor 填充颜色（32位RGBA格式）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    void setPointMarkers(const float* worldPositions, uint32_t count,
                         float markerSize, uint32_t fillColor,
                         uint32_t borderColor);

    /**
     * @brief 设置选择框
     * 
     * @param bbox 边界框
     * @param colorRGBA 颜色（32位RGBA格式）
     */
    void setSelectionBox(const BBox2f* bbox, uint32_t colorRGBA);

    /**
     * @brief 设置选择手柄
     * 
     * @param worldPositions 世界坐标数组（每点2个float）
     * @param count 手柄数量
     * @param handleSize 手柄大小（像素）
     * @param fillColor 填充颜色（32位RGBA格式）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    void setSelectionHandles(const float* worldPositions, uint32_t count,
                             float handleSize, uint32_t fillColor,
                             uint32_t borderColor);

    /**
     * @brief 设置选择预览矩形（框选/交选时的半透明填充矩形）
     * 
     * @param bbox 矩形边界
     * @param fillColor 填充颜色（32位RGBA格式，alpha=0时无填充）
     * @param borderColor 边框颜色（32位RGBA格式）
     */
    void setSelectionRect(const BBox2f* bbox, uint32_t fillColor,
                          uint32_t borderColor);

    /**
     * @brief 提交统一的叠加层图元
     *
     * 将统一描述的 overlay 图元转换为内部顶点数据。
     * 这是 Phase 1 引入的新入口，用于替代专用 set* 方法。
     *
     * @param primitive 图元描述指针
     */
    void submitOverlay(const OverlayPrimitive* primitive);

    /**
     * @brief 清除所有通过 submitOverlay 提交的图元
     */
    void clearUnifiedOverlays();

    /**
     * @brief 按生命周期分组清除通过 submitOverlay 提交的图元
     *
     * 只认 group，与几何形态（form）无关。渲染始终使用统一顶点缓冲，
     * 分组仅用于增量式 overlay 更新：先清除旧分组数据，再 submit 新数据。
     *
     * @param group 要清除的图元分组
     */
    void clearOverlayGroup(OverlayGroup group);

    /**
     * @brief 渲染所有叠加元素
     *
     * Phase 3 起，overlay 的绘制命令不再直接调用 RHI，
     * 而是通过 CommandEncoder 统一收集和排序后执行。
     *
     * @param device   RHI设备指针
     * @param encoder  统一命令编码器（Phase 3 新增）
     * @param viewMatrix 3x3视图矩阵
     */
    void render(rhi::IDevice* device, CommandEncoder* encoder,
                const float viewMatrix[9]);

    /**
     * @brief 获取 overlay 顶点缓冲区句柄
     *
     * 供 CommandEncoder::execute() 绑定使用。
     */
    rhi::BufferHandle getVertexBuffer() const { return m_vertexBuffer; }

private:
    /**
     * @brief 统一 overlay 的绘制子区间记录
     *
     * 渲染只依赖 start/count/isTriangle；group 仅用于按分组清除。
     */
    struct Range {
        uint32_t start = 0;
        uint32_t count = 0;
        uint32_t isTriangle = 0;
        uint32_t group = 0;
    };

    /// 十字准星顶点数据
    std::vector<OverlayVertex> m_crosshairVerts;
    /// 捕捉指示器顶点数据
    std::vector<OverlayVertex> m_snapVerts;
    /// 预览线顶点数据
    std::vector<OverlayVertex> m_previewVerts;
    /// 控制线顶点数据
    std::vector<OverlayVertex> m_controlVerts;
    /// 点标记顶点数据
    std::vector<OverlayVertex> m_markerVerts;
    /// 选择框顶点数据
    std::vector<OverlayVertex> m_selectionBoxVerts;
    /// 手柄顶点数据
    std::vector<OverlayVertex> m_handleVerts;
    /// 选择预览矩形填充顶点数据（三角形）
    std::vector<OverlayVertex> m_selRectFillVerts;
    /// 选择预览矩形边框顶点数据（线段）
    std::vector<OverlayVertex> m_selRectBorderVerts;

    /// 统一提交的 overlay 顶点数据（Phase 1 新增）
    std::vector<OverlayVertex> m_unifiedVerts;
    std::vector<Range> m_unifiedRanges;
    /// unified 数据在合并缓冲区中的起始偏移（用于无脏数据时直接绘制）
    uint32_t m_unifiedStart = 0;

    /// RHI设备指针
    rhi::IDevice*       m_device           = nullptr;
    /// 顶点缓冲区
    rhi::BufferHandle   m_vertexBuffer     = rhi::NullHandle;
    /// 顶点缓冲区容量
    uint32_t            m_vbCapacity        = 0;
    /// 是否有脏数据需要上传
    bool                m_dirty             = false;

    /// 缓存的合并顶点缓冲区偏移量和计数，避免每帧重建
    uint32_t m_mergedOffsets[9] = {};
    uint32_t m_mergedCounts[9] = {};

    /// 每个标记的顶点数（14=填充+边框, 8=仅边框）
    uint32_t m_markerVertsPerItem = 14;
    /// 每个手柄的顶点数（14=填充+边框, 8=仅边框）
    uint32_t m_handleVertsPerItem = 14;

    /**
     * @brief 构建标记点的填充四边形
     * 
     * @param out 输出顶点数组（需要4个顶点空间）
     * @param cx 中心点X坐标
     * @param cy 中心点Y坐标
     * @param halfSize 半尺寸
     * @param fillColor 填充颜色
     */
    void buildMarkerQuad(OverlayVertex* out, float cx, float cy,
                         float halfSize, uint32_t fillColor);

    /**
     * @brief 构建标记点的边框
     * 
     * @param out 输出顶点数组（需要8个顶点空间）
     * @param cx 中心点X坐标
     * @param cy 中心点Y坐标
     * @param halfSize 半尺寸
     * @param borderColor 边框颜色
     */
    void buildMarkerBorder(OverlayVertex* out, float cx, float cy,
                           float halfSize, uint32_t borderColor);
};

} // namespace core
} // namespace render
