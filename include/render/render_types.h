/**
 * @file render_types.h
 * @brief Render 模块的公共类型定义
 * 
 * 本文件定义了渲染系统的核心数据结构和枚举类型，包括：
 * - 图元和网格的标识符类型
 * - 图元类型枚举
 * - 顶点格式结构
 * - 图元更新和描述结构
 * - 视图描述结构
 * - 几何图形描述结构
 */
#pragma once

#include <cstdint>
#include <cstddef>

namespace render {

/// 图元唯一标识符类型
using EntityId = uint64_t;
/// 网格唯一标识符类型
using MeshId   = uint64_t;

/// 无效图元ID常量
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

/// 图元更新操作类型
enum class UpdateOp : uint8_t
{
    Add    = 0,  ///< 添加新图元
    Modify = 1,  ///< 修改现有图元
    Remove = 2,  ///< 删除图元
};

/**
 * @brief 图元更新描述结构
 * 
 * 用于批量更新图元时传递更新信息，支持添加、修改和删除操作。
 */
struct EntityUpdate
{
    UpdateOp     op;            ///< 更新操作类型
    uint8_t      _pad[3];       ///< 对齐填充，确保结构大小为16字节
    EntityId     entityId;      ///< 目标图元ID
    uint32_t     vertexCount;   ///< 顶点数量（添加/修改时有效）
    uint16_t     primitiveType; ///< 图元类型（添加/修改时有效）
    uint16_t     materialIndex; ///< 材质索引（添加/修改时有效）
};

/**
 * @brief 材质描述结构
 * 
 * 定义了图元使用的材质属性，包括线宽、点大小、颜色等。
 */
struct MaterialDesc
{
    float lineWidth;  ///< 线宽（像素）
    float pointSize;  ///< 点大小（像素）
    float color[4];   ///< RGBA颜色，范围[0,1]
    uint32_t flags;   ///< 材质标志位
};

/// 图元状态标志位枚举
enum class EntityFlags : uint32_t
{
    None       = 0,      ///< 无特殊状态
    Visible    = 1 << 0, ///< 图元可见
    Selected   = 1 << 1, ///< 图元被选中
    Highlighted = 1 << 2, ///< 图元被高亮
};

/**
 * @brief 图元描述结构
 * 
 * 完整描述一个渲染图元的属性和状态。
 */
struct EntityDesc
{
    EntityId   entityId;      ///< 图元唯一标识符
    uint32_t   vertexOffset;  ///< 在顶点缓冲区中的偏移量（字节）
    uint32_t   vertexCount;   ///< 顶点数量
    uint16_t   primitiveType; ///< 图元类型，对应PrimitiveType枚举值
    uint16_t   materialIndex; ///< 材质索引
    uint32_t   flags;         ///< 图元状态标志，见EntityFlags
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
///
/// 注意：当前仅 OpenGL 后端已实现。Vulkan / Metal / Null 仅为类型预留，
/// 尚未有对应的运行时实现。跨平台目标需要 Metal 或等价非 OpenGL 后端。
enum class BackendType : int
{
    OpenGL = 0,  ///< OpenGL 后端（当前唯一已实现的后端）
    Vulkan = 1,  ///< Vulkan 后端（预留，未实现）
    Metal  = 2,  ///< Metal 后端（Apple 平台，预留，未实现）
    Null   = 3,  ///< 空后端（用于测试，预留）
};

inline const char* backendName(BackendType b)
{
    switch (b)
    {
        case BackendType::OpenGL: return "OpenGL";
        case BackendType::Vulkan: return "Vulkan";
        case BackendType::Metal:  return "Metal";
        case BackendType::Null:   return "Null";
        default:                  return "Unknown";
    }
}

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
    float rotationDeg; ///< 旋转角度（度），默认0
    int32_t hAlign;    ///< 水平对齐：0=Left, 1=Center, 2=Right
    int32_t vAlign;    ///< 垂直对齐：0=Baseline, 1=Top, 2=Middle, 3=Bottom
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

/// 叠加层几何形态。渲染只认此轴：决定如何解析 payload 生成顶点、使用何种拓扑。
enum class OverlayForm : uint8_t
{
    LineList,       ///< 线段列表（GL_LINES，逐段独立）
    Rect,           ///< 空心矩形边框（4 边 8 顶点线段）
    FilledRect,     ///< 填充矩形（2 三角形）
    Marker,         ///< 点标记（填充方块 + 边框，用于手柄/标记点）
    SnapCircle,     ///< 捕捉指示器（圆形线框）
    Count,
};

/// 叠加层生命周期分组。清除只认此轴：任意时刻按分组整体清空，与几何形态无关。
enum class OverlayGroup : uint8_t
{
    Ui,                 ///< 通用 UI overlay（默认分组）
    Preview,            ///< 预览线（绘制过程中的临时线段）
    Control,            ///< 控制线（贝塞尔/NURBS 控制多边形）
    SelectionBox,       ///< 选择框（边框 + 填充）
    SelectionOutlines,  ///< 选择轮廓（被选中图元的高亮轮廓线）
    SelectionHandles,   ///< 选择手柄（缩放/移动/旋转手柄点）
    PointMarkers,       ///< 点标记（捕捉点、特征点标记）
    Snap,               ///< 捕捉指示器
    Count,
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

/// 叠加层图元：几何形态(渲染用) × 生命周期分组(清除用) 两个独立轴。
/// 需要视觉差异（虚实/线宽/颜色）时扩展 OverlayStyle，而不是枚举增殖。
struct OverlayPrimitive
{
    OverlayForm form;       ///< 几何形态 —— 渲染只认此字段
    OverlayGroup group;     ///< 生命周期分组 —— 清除只认此字段
    uint32_t flags;         ///< 标志位（保留）
    const void* payload;    ///< 类型相关的数据指针
    uint32_t payloadSize;   ///< payload 大小（字节）
    OverlayStyle style;     ///< 绘制样式
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

/// 点集描述（对应 OverlayForm::Marker / SnapCircle）
struct OverlayMarkerSetDesc
{
    const float* positions;     ///< 位置数组（每点2个float: x,y）
    uint32_t count;             ///< 点数量
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
    EntityId entityId;           ///< 图元ID，非0时直接使用；为0时由内部自动分配

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

// ============================================================================
// Phase 3/8: 统一命令编码公共类型
// ============================================================================

/// 绘制空间类型，用于区分数据来源
enum class DrawSpace : uint8_t
{
    World2D  = 0,  ///< 2D 文档几何空间
    Overlay  = 1,  ///< 叠加层空间
};

/// 统一的 batch key，64bit
using BatchKey = uint64_t;

/// 统一的绘制命令描述（供 CommandEncoder / DrawBatcher 共享）
struct DrawCommand
{
    BatchKey      sortKey;        ///< 排序键
    DrawSpace     space;          ///< 绘制空间
    PrimitiveType topology;       ///< 图元拓扑类型
    uint16_t      materialIndex;  ///< 材质索引（World2D 有效）
    uint32_t      zOrder;         ///< Z 序（Overlay 有效）

    union {
        struct {
            uint32_t indirectOffset;  ///< 间接命令字节偏移
            uint32_t indirectCount;   ///< 间接命令数量
        } world;
        struct {
            uint32_t vertexOffset;  ///< 顶点偏移
            uint32_t vertexCount;   ///< 顶点数量
        } overlay;
    };
    float lineWidth = 1.0f;  ///< 线宽（World2D 有效）
};

/// 屏幕空间文本项（用于屏幕坐标文本渲染）
struct ScreenTextItem
{
    const char* text;       ///< 文本内容（UTF-8）
    float x;                 ///< 屏幕坐标 X（像素）
    float y;                 ///< 屏幕坐标 Y（像素）
    float color[4];          ///< RGBA 颜色
    float fontSize;          ///< 字体大小（像素）
};

}
