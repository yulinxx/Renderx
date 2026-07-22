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

/**
 * @brief 叠加层渲染队列类
 * 
 * 管理各种叠加元素的顶点数据，批量上传并渲染。
 */
class OverlayQueue {
public:
    /**
     * @brief 叠加层顶点结构
     * 
     * 包含位置和颜色信息，用于渲染叠加元素。
     */
    struct OverlayVertex {
        float px, py, pz;    ///< 位置坐标（世界空间）
        float cr, cg, cb, ca; ///< 颜色（RGBA，范围0-1）
    };

    /// 静态断言：OverlayVertex 大小必须为28字节
    static_assert(sizeof(OverlayVertex) == 28, "OverlayVertex must be 28 bytes");

    /**
     * @brief 初始化叠加层渲染器
     * 
     * @param device RHI设备指针
     */
    void initialize(rhi::IDevice* device);

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
     * @brief 渲染所有叠加元素
     * 
     * @param device RHI设备指针
     * @param viewMatrix 3x3视图矩阵
     */
    void render(rhi::IDevice* device, const float viewMatrix[9]);

private:
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

    /// RHI设备指针
    rhi::IDevice*       m_device           = nullptr;
    /// 顶点缓冲区
    rhi::BufferHandle   m_vertexBuffer     = rhi::NullHandle;
    /// 线渲染管线
    rhi::PipelineHandle m_linePipeline      = {};
    /// 三角形渲染管线
    rhi::PipelineHandle m_trianglePipeline  = {};
    /// 顶点缓冲区容量
    uint32_t            m_vbCapacity        = 0;
    /// 是否有脏数据需要上传
    bool                m_dirty             = false;

    /// 缓存的合并顶点缓冲区偏移量和计数，避免每帧重建
    uint32_t m_mergedOffsets[7] = {};
    uint32_t m_mergedCounts[7] = {};

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

    /**
     * @brief 上传顶点数据并渲染
     * 
     * @param device RHI设备指针
     * @param data 顶点数据
     * @param vertexCount 顶点数量
     * @param type 图元类型
     */
    void uploadAndRender(rhi::IDevice* device, const OverlayVertex* data,
                         uint32_t vertexCount, PrimitiveType type);
};

} // namespace core
} // namespace render
