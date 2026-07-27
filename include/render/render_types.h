/**
 * @file render_types.h
 * @brief Render 模块的公共类型定义
 * 
 * 本文件定义了渲染系统的核心数据结构和枚举类型，包括：
 * - 实体和网格的标识符类型
 * - 图元类型枚举
 * - 顶点格式结构
 * - 实体更新和描述结构
 * - 视图描述结构
 * - 几何图形描述结构
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace render {

/// 实体唯一标识符类型
using EntityId = uint64_t;
/// 网格唯一标识符类型
using MeshId   = uint64_t;

/// 无效实体ID常量
constexpr EntityId INVALID_ENTITY_ID = 0;
/// 无效网格ID常量
constexpr MeshId   INVALID_MESH_ID   = 0;

/// 图元类型枚举，定义了支持的渲染图元类型
enum class PrimitiveType : uint8_t
{
    PointList     = 0,  ///< 点列表，每个顶点单独渲染为一个点
    LineList      = 1,  ///< 线列表，每两个顶点组成一条线段
    LineStrip     = 2,  ///< 线带，连续顶点组成连续线段
    LineLoop      = 3,  ///< 环线，线带的首尾相连形成封闭环
    TriangleList  = 4,  ///< 三角形列表，每三个顶点组成一个三角形
    TriangleStrip = 5,  ///< 三角形带，连续顶点组成连续三角形
    TriangleFan   = 6,  ///< 三角形扇，以第一个顶点为中心向外辐射
};

/// 图元类型的总数
constexpr uint32_t PRIMITIVE_TYPE_COUNT = 7;

/**
 * @brief 3D位置 + 3通道颜色顶点格式
 * 
 * 每个顶点包含3个浮点数的位置坐标和3个浮点数的颜色通道。
 * 总大小：24字节（3*4 + 3*4）。
 */
struct VertexP3C3
{
    float px, py, pz;  ///< 顶点位置坐标 (x, y, z)
    float cr, cg, cb;  ///< 顶点颜色 (r, g, b)，范围[0,1]
};

static_assert(sizeof(VertexP3C3) == 24, "VertexP3C3 must be 24 bytes");

/**
 * @brief 3D位置 + 3通道法向量顶点格式
 * 
 * 每个顶点包含3个浮点数的位置坐标和3个浮点数的法向量。
 * 总大小：24字节（3*4 + 3*4）。
 * 用于3D网格的渲染。
 */
struct VertexP3N3
{
    float px, py, pz;  ///< 顶点位置坐标 (x, y, z)
    float nx, ny, nz;  ///< 顶点法向量 (nx, ny, nz)，单位向量
};

static_assert(sizeof(VertexP3N3) == 24, "VertexP3N3 must be 24 bytes");

/// 实体更新操作类型
enum class UpdateOp : uint8_t
{
    Add    = 0,  ///< 添加新实体
    Modify = 1,  ///< 修改现有实体
    Remove = 2,  ///< 删除实体
};

/**
 * @brief 实体更新描述结构
 * 
 * 用于批量更新实体时传递更新信息，支持添加、修改和删除操作。
 */
struct EntityUpdate
{
    UpdateOp     op;            ///< 更新操作类型
    uint8_t      _pad[3];       ///< 对齐填充，确保结构大小为16字节
    EntityId     entityId;      ///< 目标实体ID
    uint32_t     vertexCount;   ///< 顶点数量（添加/修改时有效）
    uint16_t     primitiveType; ///< 图元类型（添加/修改时有效）
    uint16_t     materialIndex; ///< 材质索引（添加/修改时有效）
};

/**
 * @brief 材质描述结构
 * 
 * 定义了实体使用的材质属性，包括线宽、点大小、颜色等。
 */
struct MaterialDesc
{
    float lineWidth;  ///< 线宽（像素）
    float pointSize;  ///< 点大小（像素）
    float color[4];   ///< RGBA颜色，范围[0,1]
    uint32_t flags;   ///< 材质标志位
};

/// 实体状态标志位枚举
enum class EntityFlags : uint32_t
{
    None       = 0,      ///< 无特殊状态
    Visible    = 1 << 0, ///< 实体可见
    Selected   = 1 << 1, ///< 实体被选中
    Highlighted = 1 << 2, ///< 实体被高亮
};

/**
 * @brief 实体描述结构
 * 
 * 完整描述一个渲染实体的属性和状态。
 */
struct EntityDesc
{
    EntityId   entityId;      ///< 实体唯一标识符
    uint32_t   vertexOffset;  ///< 在顶点缓冲区中的偏移量（字节）
    uint32_t   vertexCount;   ///< 顶点数量
    uint16_t   primitiveType; ///< 图元类型，对应PrimitiveType枚举值
    uint16_t   materialIndex; ///< 材质索引
    uint32_t   flags;         ///< 实体状态标志，见EntityFlags
    float      boundingBox[4]; ///< 2D边界框 [minX, minY, maxX, maxY]
};

/**
 * @brief 网格描述结构
 * 
 * 描述3D网格在顶点/索引缓冲区中的位置和范围。
 */
struct MeshDesc
{
    uint32_t indexOffset;  ///< 在索引缓冲区中的偏移量
    uint32_t indexCount;   ///< 索引数量
    uint32_t vertexOffset; ///< 在顶点缓冲区中的偏移量
    uint32_t vertexCount;  ///< 顶点数量
};

/**
 * @brief 实例描述结构
 * 
 * 用于3D网格的实例化渲染，每个实例有独立的模型矩阵和材质。
 */
struct InstanceDesc
{
    float    modelMatrix[16]; ///< 4x4模型矩阵（列主序），偏移0
    float    color[4];        ///< RGBA颜色，偏移64
    uint32_t flags;           ///< 实例状态标志，偏移80
    uint32_t meshIndex;       ///< 对应的网格索引，偏移84
    uint32_t materialIndex;   ///< 材质索引，偏移88
    uint32_t _pad;            ///< 对齐填充至96字节，偏移92
};

/**
 * @brief 间接绘制命令结构
 * 
 * 用于OpenGL的glDrawArraysIndirect调用，存储绘制参数。
 */
struct DrawIndirectCmd
{
    uint32_t vertexCount;   ///< 顶点数量
    uint32_t instanceCount; ///< 实例数量
    uint32_t firstVertex;   ///< 起始顶点索引
    uint32_t baseInstance;  ///< 基础实例索引
};

/**
 * @brief 索引间接绘制命令结构
 * 
 * 用于OpenGL的glDrawElementsIndirect调用，存储带索引的绘制参数。
 */
struct DrawIndexedIndirectCmd
{
    uint32_t indexCount;    ///< 索引数量
    uint32_t instanceCount; ///< 实例数量
    uint32_t firstIndex;    ///< 起始索引
    int32_t  vertexOffset;  ///< 顶点偏移
    uint32_t baseInstance;  ///< 基础实例索引
};

/// 渲染后端类型枚举
enum class BackendType : int
{
    OpenGL = 0,  ///< OpenGL 后端
    Vulkan = 1,  ///< Vulkan 后端
    Metal  = 2,  ///< Metal 后端（Apple平台）
    Null   = 3,  ///< 空后端（用于测试）
};

/// 视图模式枚举
enum class ViewMode : int
{
    Mode2D = 0,  ///< 2D视图模式
    Mode3D = 1,  ///< 3D视图模式
};

/**
 * @brief 设备创建描述结构
 * 
 * 用于创建渲染设备时传递初始化参数。
 */
struct DeviceDesc
{
    BackendType backend;          ///< 渲染后端类型
    bool        debugLayer;       ///< 是否启用调试层
    uint8_t     _pad[2];          ///< 对齐填充
    void*       nativeWindowHandle; ///< 原生窗口句柄（如HWND）
    uint32_t    width;            ///< 渲染目标宽度（像素）
    uint32_t    height;           ///< 渲染目标高度（像素）
};

/**
 * @brief 2D视图描述结构
 * 
 * 描述2D视图的变换矩阵和视口尺寸。
 */
struct ViewDesc2D
{
    float viewMatrix[9]; ///< 3x3视图变换矩阵（列主序）
    float viewWidth;     ///< 视图宽度（世界坐标单位）
    float viewHeight;    ///< 视图高度（世界坐标单位）
};

/**
 * @brief 3D视图描述结构
 * 
 * 描述3D视图的视图矩阵和投影矩阵。
 */
struct ViewDesc3D
{
    float viewMatrix[16]; ///< 4x4视图矩阵（列主序）
    float projMatrix[16]; ///< 4x4投影矩阵（列主序）
};

/**
 * @brief 覆盖层数据结构
 * 
 * 存储2D视图中的覆盖元素数据，如十字准星、捕捉点等。
 */
struct OverlayData
{
    float  crosshairWorld[2]; ///< 十字准星的世界坐标位置
    int32_t crosshairVisible; ///< 十字准星是否可见（0/1）
    float  snapWorld[2];      ///< 捕捉点的世界坐标位置
    int32_t snapVisible;      ///< 捕捉点是否可见（0/1）
    float  snapColor[4];      ///< 捕捉点颜色 RGBA
    float  mouseWorld[2];     ///< 鼠标的世界坐标位置
};

/**
 * @brief 文本项描述结构
 * 
 * 描述单个文本元素的渲染属性。
 */
struct TextItem
{
    const char* text;       ///< 文本内容（UTF-8）
    float       x, y;       ///< 文本位置（世界坐标或屏幕坐标）
    int32_t     coordMode;  ///< 坐标模式：0=世界坐标，1=屏幕坐标
    int32_t     hAlign;     ///< 水平对齐：0=左对齐，1=居中，2=右对齐
    int32_t     vAlign;     ///< 垂直对齐：0=上对齐，1=居中，2=下对齐
    int32_t     fontSize;   ///< 字体大小（像素）
    float       color[4];   ///< 文本颜色 RGBA
    float       rotationDeg; ///< 旋转角度（度）
    float       zOrder;     ///< Z序（用于叠加层排序）
};

/**
 * @brief 文本项列表结构
 * 
 * 用于批量提交多个文本项进行渲染。
 */
struct TextItemList
{
    const TextItem* items; ///< 文本项数组指针
    uint32_t        count; ///< 文本项数量
};

/**
 * @brief 2D浮点数边界框结构
 */
struct BBox2f
{
    float minX, minY, maxX, maxY; ///< 边界框的最小和最大坐标
};

/**
 * @brief 2D双精度向量结构
 */
struct Vec2d
{
    double x, y; ///< 向量的x和y分量
};

/**
 * @brief 折线几何结构
 */
struct GeometryPolyline
{
    const Vec2d* points;   ///< 顶点数组指针
    uint32_t pointCount;   ///< 顶点数量
    bool closed;           ///< 是否闭合（首尾相连）
    float color[4];        ///< RGBA颜色，默认白色
};

/**
 * @brief 圆形几何结构
 */
struct GeometryCircle
{
    Vec2d center;    ///< 圆心坐标
    double radius;   ///< 半径
    float color[4];  ///< RGBA颜色，默认白色
};

/**
 * @brief 圆弧几何结构
 */
struct GeometryArc
{
    Vec2d center;       ///< 圆心坐标
    double radius;      ///< 半径
    double startAngle;  ///< 起始角度（弧度）
    double endAngle;    ///< 结束角度（弧度）
    float color[4];     ///< RGBA颜色，默认白色
};

/**
 * @brief 椭圆几何结构
 */
struct GeometryEllipse
{
    Vec2d center;       ///< 椭圆中心坐标
    double radiusX;     ///< X轴半径
    double radiusY;     ///< Y轴半径
    double rotation;    ///< 旋转角度（弧度）
    double startAngle;  ///< 起始角度（弧度）
    double endAngle;    ///< 结束角度（弧度）
    bool fullEllipse;   ///< 是否为完整椭圆
    float color[4];     ///< RGBA颜色，默认白色
};

/**
 * @brief 文本几何结构
 */
struct GeometryText
{
    Vec2d position;    ///< 文本位置
    const char* text;  ///< 文本内容（UTF-8）
    float color[4];    ///< RGBA颜色，默认白色
    float fontSize;    ///< 字体大小（像素），默认12
};

/**
 * @brief 图像几何结构
 * 
 * 使用四个角点定义一个任意四边形的图像区域。
 */
struct GeometryImage
{
    Vec2d topLeft;     ///< 左上角坐标
    Vec2d topRight;    ///< 右上角坐标
    Vec2d bottomLeft;  ///< 左下角坐标
    Vec2d bottomRight; ///< 右下角坐标
    float color[4];    ///< RGBA边框颜色，默认白色
};

/// 叠加层图元类型枚举，统一描述所有 2D overlay 元素
enum class OverlayPrimitiveKind : uint8_t
{
    LineList,       ///< 线段列表（preview lines, control lines, selection box border）
    Rect,           ///< 空心矩形（selection box, selection rect border）
    FilledRect,     ///< 填充矩形（selection rect fill）
    Points,         ///< 点集（point markers, selection handles）
    Crosshair,      ///< 十字准星
    SnapIndicator,  ///< 捕捉指示器（圆形）
};

/// 叠加层绘制样式
struct OverlayStyle
{
    uint32_t fillColor = 0;     ///< 填充颜色（RGBA格式）
    uint32_t borderColor = 0;   ///< 边框/线条颜色（RGBA格式）
    float lineWidth = 1.0f;     ///< 线宽（像素）
    float pointSize = 8.0f;     ///< 点/标记大小（像素）
    float zOrder = 0.0f;        ///< Z序（用于叠加层排序）
};

/// 线段列表描述（对应 LineList）
struct OverlayPolylineDesc
{
    const float* vertices;      ///< 顶点位置数组（每顶点3个float: x,y,z）
    uint32_t vertexCount;       ///< 顶点数量
    bool usePerVertexColor;     ///< 是否使用逐顶点颜色（为true时colors字段有效）
    const float* colors;        ///< 逐顶点颜色数组（每顶点3个float: r,g,b），可选
};

/// 矩形描述（对应 Rect / FilledRect）
struct OverlayRectDesc
{
    float minX, minY;           ///< 左上角坐标
    float maxX, maxY;           ///< 右下角坐标
};

/// 点集描述（对应 Points）
struct OverlayMarkerSetDesc
{
    const float* positions;     ///< 位置数组（每点2个float: x,y）
    uint32_t count;             ///< 点数量
};

/// 统一的叠加层图元描述
struct OverlayPrimitive
{
    OverlayPrimitiveKind kind;  ///< 图元类型
    uint32_t flags;             ///< 标志位（保留）
    const void* payload;        ///< 类型相关的数据指针
    uint32_t payloadSize;       ///< payload 大小（字节）
    OverlayStyle style;         ///< 绘制样式
};

// ============================================================================
// Phase 2: 统一几何提交模型
// ============================================================================

/// 几何图元类型枚举，统一描述所有可提交的几何内容
enum class GeometryPrimitiveKind : uint8_t
{
    Polyline     = 0,  ///< 多段线（2D 文档几何路径）
    Circle       = 1,  ///< 圆形（2D 文档几何路径）
    Arc          = 2,  ///< 圆弧（2D 文档几何路径）
    Ellipse      = 3,  ///< 椭圆（2D 文档几何路径）
    Image        = 4,  ///< 图像线框（2D 文档几何路径）
    Text         = 5,  ///< 文本（文本缓存 / TextAtlas 路径）
    TriangleSoup = 6,  ///< 三角网格（3D mesh 路径）
};

/// 多段线几何描述（对应 GeometryPrimitiveKind::Polyline）
/// 复用现有 GeometryPolyline，此处仅做类型别名说明
// typedef GeometryPolyline GeometryPolylineDesc;

/// 圆形几何描述（对应 GeometryPrimitiveKind::Circle）
/// 复用现有 GeometryCircle

/// 圆弧几何描述（对应 GeometryPrimitiveKind::Arc）
/// 复用现有 GeometryArc

/// 椭圆几何描述（对应 GeometryPrimitiveKind::Ellipse）
/// 复用现有 GeometryEllipse

/// 文本几何描述（对应 GeometryPrimitiveKind::Text）
/// 复用现有 GeometryText

/// 图像几何描述（对应 GeometryPrimitiveKind::Image）
/// 复用现有 GeometryImage

/// 三角网格几何描述（对应 GeometryPrimitiveKind::TriangleSoup）
struct GeometryTriangleSoupDesc
{
    const float* vertices;     ///< 顶点位置数组（每顶点3个float: x,y,z）
    const float* normals;      ///< 顶点法线数组（每顶点3个float: nx,ny,nz）
    uint32_t vertexCount;      ///< 顶点数量
    float color[4];            ///< RGBA颜色，范围[0,1]
};

/**
 * @brief 统一几何图元描述
 *
 * Phase 2 引入的统一几何提交入口使用此结构。
 * 通过 kind 字段区分类型，union 提供类型明确的描述指针，
 * 避免纯 void* payload 导致的类型信息丢失。
 *
 * 内部分发规则：
 * - Polyline / Circle / Arc / Ellipse / Image → 2D 文档几何路径（world2D）
 * - Text → 文本缓存路径（TextAtlas）
 * - TriangleSoup → 3D mesh 路径（MeshManager）
 */
struct GeometryPrimitive
{
    GeometryPrimitiveKind kind;  ///< 图元类型
    uint32_t flags;              ///< 标志位（保留）

    /// 类型明确的描述指针联合体
    union {
        const GeometryPolyline*     polyline;     ///< Polyline 类型的描述
        const GeometryCircle*       circle;       ///< Circle 类型的描述
        const GeometryArc*          arc;          ///< Arc 类型的描述
        const GeometryEllipse*      ellipse;      ///< Ellipse 类型的描述
        const GeometryImage*        image;        ///< Image 类型的描述
        const GeometryText*         text;         ///< Text 类型的描述
        const GeometryTriangleSoupDesc* triangleSoup; ///< TriangleSoup 类型的描述
    } desc;
};

}
