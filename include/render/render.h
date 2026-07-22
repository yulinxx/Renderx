/**
 * @file render.h
 * @brief Render 模块的公共 API 头文件
 * 
 * 本文件定义了渲染模块对外暴露的 C 接口，包括：
 * - 设备创建和销毁
 * - 实体管理（添加、修改、删除）
 * - 网格和实例管理
 * - 材质管理
 * - 视图设置
 * - 覆盖层和文本渲染
 * - 场景环境渲染
 * - 帧渲染
 * 
 * 所有接口都使用 C 语言调用约定，便于跨语言绑定和动态库使用。
 */
#pragma once

#include "render_types.h"
#include <cstdint>

/// API 导出宏定义，支持 Windows 和 Linux/macOS 平台
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

/**
 * @brief 渲染统计信息结构
 * 
 * 用于查询当前帧的渲染统计数据。
 */
struct RenderStats
{
    uint32_t entityCount;      ///< 当前场景中的实体总数
    uint32_t visibleCount;     ///< 当前帧可见的实体数量
    uint32_t drawCallCount;    ///< 当前帧的绘制调用次数
    uint32_t triangleCount;    ///< 当前帧绘制的三角形数量
    uint32_t lineCount;        ///< 当前帧绘制的线段数量
    uint32_t pointCount;       ///< 当前帧绘制的点数量
    uint64_t gpuMemoryBytes;   ///< 当前 GPU 内存使用量（字节）
};

/// 渲染设备的前向声明
typedef struct RenderDevice RenderDevice;

extern "C" {

/**
 * @brief 创建渲染设备
 * 
 * @param desc 设备描述结构，包含后端类型、窗口句柄等参数
 * @return 渲染设备指针，失败返回 nullptr
 */
RENDER_API RenderDevice* renderCreateDevice(const DeviceDesc* desc);

/**
 * @brief 销毁渲染设备
 * 
 * @param dev 要销毁的渲染设备
 */
RENDER_API void          renderDestroyDevice(RenderDevice* dev);

/**
 * @brief 调整渲染目标尺寸
 * 
 * @param dev 渲染设备
 * @param width 新的宽度（像素）
 * @param height 新的高度（像素）
 */
RENDER_API void renderResize(RenderDevice* dev, uint32_t width, uint32_t height);

/**
 * @brief 添加2D实体到场景
 * 
 * @param dev 渲染设备
 * @param id 实体唯一标识符
 * @param vertices 顶点数据数组
 * @param vertexCount 顶点数量
 * @param type 图元类型
 * @param materialIdx 材质索引
 * @return 实体在内部的索引位置
 */
RENDER_API uint32_t renderAddEntity(RenderDevice* dev, EntityId id,
                                    const VertexP3C3* vertices, uint32_t vertexCount,
                                    PrimitiveType type, uint16_t materialIdx);

/**
 * @brief 修改已存在的2D实体
 * 
 * @param dev 渲染设备
 * @param id 要修改的实体ID
 * @param vertices 新的顶点数据
 * @param vertexCount 顶点数量
 * @param materialIdx 材质索引
 */
RENDER_API void     renderModifyEntity(RenderDevice* dev, EntityId id,
                                       const VertexP3C3* vertices, uint32_t vertexCount,
                                       uint16_t materialIdx);

/**
 * @brief 从场景中删除2D实体
 * 
 * @param dev 渲染设备
 * @param id 要删除的实体ID
 */
RENDER_API void     renderRemoveEntity(RenderDevice* dev, EntityId id);

/**
 * @brief 设置实体的可见性
 * 
 * @param dev 渲染设备
 * @param id 实体ID
 * @param visible 可见性：0=不可见，1=可见
 */
RENDER_API void     renderSetEntityVisibility(RenderDevice* dev, EntityId id, int32_t visible);

/**
 * @brief 批量应用实体更新
 * 
 * @param dev 渲染设备
 * @param packet 更新数据包指针
 * @param packetSize 数据包大小（字节）
 */
RENDER_API void renderApplyUpdates(RenderDevice* dev, const void* packet, uint32_t packetSize);

/**
 * @brief 注册3D网格到渲染系统
 * 
 * @param dev 渲染设备
 * @param positions 顶点位置数组（每顶点3个float）
 * @param normals 顶点法向量数组（每顶点3个float，可为null）
 * @param indices 索引数组（可为null）
 * @param vertexCount 顶点数量
 * @param indexCount 索引数量
 * @return 网格ID，失败返回INVALID_MESH_ID
 */
RENDER_API MeshId renderRegisterMesh(RenderDevice* dev,
                                     const float* positions, const float* normals,
                                     const uint32_t* indices,
                                     uint32_t vertexCount, uint32_t indexCount);

/**
 * @brief 从渲染系统中注销3D网格
 * 
 * @param dev 渲染设备
 * @param mesh 要注销的网格ID
 */
RENDER_API void   renderUnregisterMesh(RenderDevice* dev, MeshId mesh);

/**
 * @brief 添加3D网格实例
 * 
 * @param dev 渲染设备
 * @param mesh 网格ID
 * @param modelMatrix 4x4模型矩阵（列主序）
 * @param materialIdx 材质索引
 * @return 实例ID
 */
RENDER_API uint32_t renderAddInstance(RenderDevice* dev, MeshId mesh,
                                      const float modelMatrix[16], uint32_t materialIdx,
                                      const float color[4]);

/**
 * @brief 修改3D网格实例
 * 
 * @param dev 渲染设备
 * @param instanceId 实例ID
 * @param modelMatrix 新的4x4模型矩阵（列主序）
 */
RENDER_API void     renderModifyInstance(RenderDevice* dev, uint32_t instanceId,
                                         const float modelMatrix[16]);

/**
 * @brief 删除3D网格实例
 * 
 * @param dev 渲染设备
 * @param instanceId 要删除的实例ID
 */
RENDER_API void     renderRemoveInstance(RenderDevice* dev, uint32_t instanceId);

/**
 * @brief 添加材质到渲染系统
 * 
 * @param dev 渲染设备
 * @param desc 材质描述结构
 * @return 材质索引
 */
RENDER_API uint16_t renderAddMaterial(RenderDevice* dev, const MaterialDesc* desc);

/**
 * @brief 更新已有材质
 * 
 * @param dev 渲染设备
 * @param idx 材质索引
 * @param desc 新的材质描述
 */
RENDER_API void     renderUpdateMaterial(RenderDevice* dev, uint16_t idx, const MaterialDesc* desc);

/**
 * @brief 设置2D视图参数
 * 
 * @param dev 渲染设备
 * @param viewMatrix 3x3视图变换矩阵（列主序）
 * @param viewWidth 视图宽度（世界坐标单位）
 * @param viewHeight 视图高度（世界坐标单位）
 */
RENDER_API void renderSetView2D(RenderDevice* dev, const float viewMatrix[9],
                                float viewWidth, float viewHeight);

/**
 * @brief 设置3D视图参数
 * 
 * @param dev 渲染设备
 * @param viewMatrix 4x4视图矩阵（列主序）
 * @param projMatrix 4x4投影矩阵（列主序）
 */
RENDER_API void renderSetView3D(RenderDevice* dev, const float viewMatrix[16],
                                const float projMatrix[16]);

/**
 * @brief 设置视图模式（2D/3D）
 * 
 * 切换视图模式会影响背景色和渲染状态：
 * - Mode2D: 浅灰色背景，禁用深度测试
 * - Mode3D: 深蓝色背景，启用深度测试
 * 
 * @param dev 渲染设备
 * @param mode 视图模式
 */
RENDER_API void renderSetViewMode(RenderDevice* dev, ViewMode mode);

/**
 * @brief 设置覆盖层数据
 * 
 * @param dev 渲染设备
 * @param overlay 覆盖层数据结构（十字准星、捕捉点等）
 */
RENDER_API void renderSetOverlay(RenderDevice* dev, const OverlayData* overlay);

/**
 * @brief 设置要渲染的文本列表
 * 
 * @param dev 渲染设备
 * @param texts 文本项列表结构
 */
RENDER_API void renderSetTexts(RenderDevice* dev, const TextItemList* texts);

/**
 * @brief 设置预览线（用于绘制正在创建的几何图形）
 * 
 * @param dev 渲染设备
 * @param vertices 顶点数据
 * @param vertexCount 顶点数量
 * @param colorRGBA 颜色（RGBA格式，每个通道8位）
 */
RENDER_API void renderSetPreviewLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA);

/**
 * @brief 设置控制线（用于绘制控制点连线）
 * 
 * @param dev 渲染设备
 * @param vertices 顶点数据
 * @param vertexCount 顶点数量
 * @param colorRGBA 颜色（RGBA格式，每个通道8位）
 */
RENDER_API void renderSetControlLines(RenderDevice* dev, const VertexP3C3* vertices,
                                      uint32_t vertexCount, uint32_t colorRGBA);

/**
 * @brief 设置点标记（用于标记关键点位置）
 * 
 * @param dev 渲染设备
 * @param worldPositions 世界坐标位置数组（每点2个float）
 * @param count 点数量
 * @param markerSize 标记大小（像素）
 * @param fillColor 填充颜色（RGBA格式）
 * @param borderColor 边框颜色（RGBA格式）
 */
RENDER_API void renderSetPointMarkers(RenderDevice* dev, const float* worldPositions,
                                      uint32_t count, float markerSize,
                                      uint32_t fillColor, uint32_t borderColor);

/**
 * @brief 设置选择框（用于显示选中区域）
 * 
 * @param dev 渲染设备
 * @param bbox 边界框
 * @param colorRGBA 颜色（RGBA格式）
 */
RENDER_API void renderSetSelectionBox(RenderDevice* dev, const BBox2f* bbox, uint32_t colorRGBA);

/**
 * @brief 设置选择手柄（用于拖拽调整选中对象）
 * 
 * @param dev 渲染设备
 * @param worldPositions 世界坐标位置数组（每点2个float）
 * @param count 手柄数量
 * @param handleSize 手柄大小（像素）
 * @param fillColor 填充颜色（RGBA格式）
 * @param borderColor 边框颜色（RGBA格式）
 */
RENDER_API void renderSetSelectionHandles(RenderDevice* dev, const float* worldPositions,
                                          uint32_t count, float handleSize,
                                          uint32_t fillColor, uint32_t borderColor);

/**
 * @brief 设置场景环境层（如网格背景、参考线等）
 * 
 * @param dev 渲染设备
 * @param vertices 顶点数据
 * @param vertexCount 顶点数量
 * @param layerOffsets 各层的顶点偏移数组
 * @param layerCount 层数
 * @param layerColors 各层的颜色数组（RGBA格式）
 * @param layerWidths 各层的线宽数组
 */
RENDER_API void renderSetSceneEnv(RenderDevice* dev, const VertexP3C3* vertices,
                                  uint32_t vertexCount, const uint32_t* layerOffsets,
                                  uint32_t layerCount, const uint32_t* layerColors,
                                  const float* layerWidths);

/**
 * @brief 设置场景环境层（完整版本，支持像素坐标）
 * 
 * @param dev 渲染设备
 * @param vertices 顶点数据
 * @param vertexCount 顶点数量
 * @param layerOffsets 各层的顶点偏移数组
 * @param layerCount 层数
 * @param layerColors 各层的颜色数组（RGBA格式）
 * @param layerWidths 各层的线宽数组
 * @param pixelFlags 各层是否使用像素坐标的标志数组
 */
RENDER_API void renderSetSceneEnvEx(RenderDevice* dev, const VertexP3C3* vertices,
                                    uint32_t vertexCount, const uint32_t* layerOffsets,
                                    uint32_t layerCount, const uint32_t* layerColors,
                                    const float* layerWidths, const bool* pixelFlags);

/**
 * @brief 设置位图图像（用于显示图片覆盖层）
 * 
 * @param dev 渲染设备
 * @param rgba RGBA像素数据
 * @param w 图像宽度
 * @param h 图像高度
 * @param tlX 左上角X坐标（世界坐标）
 * @param tlY 左上角Y坐标（世界坐标）
 * @param trX 右上角X坐标（世界坐标）
 * @param trY 右上角Y坐标（世界坐标）
 * @param blX 左下角X坐标（世界坐标）
 * @param blY 左下角Y坐标（世界坐标）
 * @param brX 右下角X坐标（世界坐标）
 * @param brY 右下角Y坐标（世界坐标）
 */
RENDER_API void renderSetBitmap(RenderDevice* dev, const uint8_t* rgba, int32_t w, int32_t h,
                                float tlX, float tlY, float trX, float trY,
                                float blX, float blY, float brX, float brY);

/**
 * @brief 清除位图图像
 * 
 * @param dev 渲染设备
 */
RENDER_API void renderClearBitmap(RenderDevice* dev);

/**
 * @brief 执行一帧渲染
 * 
 * 此函数会执行完整的渲染流程，包括：
 * - 更新可见性查询
 * - 构建绘制批次
 * - 上传数据到GPU
 * - 执行绘制调用
 * 
 * @param dev 渲染设备
 */
RENDER_API void renderFrame(RenderDevice* dev);

/**
 * @brief 获取渲染统计信息
 * 
 * @param dev 渲染设备
 * @param stats 输出参数，用于存储统计信息
 */
RENDER_API void renderGetStats(RenderDevice* dev, RenderStats* stats);

/**
 * @brief 获取场景中的实体数量
 * 
 * @param dev 渲染设备
 * @return 实体总数
 */
RENDER_API uint32_t renderGetEntityCount(RenderDevice* dev);

/**
 * @brief 获取GPU内存使用量
 * 
 * @param dev 渲染设备
 * @return GPU内存使用量（字节）
 */
RENDER_API uint64_t renderGetGPUMemoryUsage(RenderDevice* dev);

/**
 * @brief 获取原生渲染上下文
 * 
 * @param dev 渲染设备
 * @return 原生上下文指针（如OpenGL的HGLRC）
 */
RENDER_API void* renderGetNativeContext(RenderDevice* dev);

/**
 * @brief 开始场景渲染（内部使用）
 * 
 * @param dev 渲染设备
 */
RENDER_API void renderBeginScene(RenderDevice* dev);

/**
 * @brief 结束场景渲染（内部使用）
 * 
 * @param dev 渲染设备
 */
RENDER_API void renderEndScene(RenderDevice* dev);

/**
 * @brief 发射折线几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param polyline 折线几何描述
 */
RENDER_API void renderEmitPolyline(RenderDevice* dev, const GeometryPolyline* polyline);

/**
 * @brief 发射圆形几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param circle 圆形几何描述
 */
RENDER_API void renderEmitCircle(RenderDevice* dev, const GeometryCircle* circle);

/**
 * @brief 发射圆弧几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param arc 圆弧几何描述
 */
RENDER_API void renderEmitArc(RenderDevice* dev, const GeometryArc* arc);

/**
 * @brief 发射椭圆几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param ellipse 椭圆几何描述
 */
RENDER_API void renderEmitEllipse(RenderDevice* dev, const GeometryEllipse* ellipse);

/**
 * @brief 发射文本几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param text 文本几何描述
 */
RENDER_API void renderEmitText(RenderDevice* dev, const GeometryText* text);

/**
 * @brief 发射图像几何（用于即时渲染）
 * 
 * @param dev 渲染设备
 * @param image 图像几何描述
 */
RENDER_API void renderEmitImage(RenderDevice* dev, const GeometryImage* image);

/**
 * @brief 发射三角网格（用于即时渲染）
 * 
 * 将三角网格注册到 MeshManager，并添加一个实例（单位矩阵变换）。
 * 每 3 个顶点定义一个三角形。
 * 
 * @param dev 渲染设备
 * @param vertices 顶点位置数组（每顶点3个float）
 * @param normals 顶点法线数组（每顶点3个float）
 * @param vertexCount 顶点数量
 */
RENDER_API void renderEmitTriangleSoup(RenderDevice* dev,
                                       const float* vertices, const float* normals,
                                       uint32_t vertexCount,
                                       const float color[4]);

}

}
