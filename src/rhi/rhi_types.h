/**
 * @file rhi_types.h
 * @brief Render Hardware Interface (RHI) 的类型定义
 * 
 * 本文件定义了 RHI 层的核心数据类型，包括：
 * - 纹理格式枚举
 * - 缓冲区用途枚举
 * - 内存类型枚举
 * - 图元拓扑枚举
 * - 比较函数和混合因子枚举
 * - 顶点格式枚举
 * - 各种描述结构（缓冲区、纹理、管线）
 * - 句柄类型定义
 * 
 * RHI 层提供了跨平台的图形硬件抽象，屏蔽了不同图形 API 的差异。
 */
#pragma once
#include <cstdint>

namespace render::rhi {

/// 纹理和缓冲区数据格式枚举
enum class Format : uint8_t {
    RGBA8,     ///< 4通道8位整数 RGBA
    RGBA32F,   ///< 4通道32位浮点 RGBA
    RG32F,     ///< 2通道32位浮点 RG
    R32F,      ///< 1通道32位浮点 R
    D32F,      ///< 32位浮点深度
    D24S8,     ///< 24位深度 + 8位模板
    R8,        ///< 1通道8位整数 R
};

/// 缓冲区用途枚举，用于指定缓冲区的绑定目标
enum class BufferUsage : uint8_t {
    Vertex,         ///< 顶点缓冲区
    Index,          ///< 索引缓冲区
    Uniform,        ///< 统一缓冲区（Uniform Buffer）
    Indirect,       ///< 间接绘制命令缓冲区
    ShaderVisible,  ///< 着色器可见的缓冲区（如实例数据）
    Staging,        ///< 暂存缓冲区（用于CPU-GPU数据传输）
};

/// 内存类型枚举，用于指定缓冲区/纹理的内存分配策略
enum class MemoryType : uint8_t {
    GPU_Only,          ///< 仅GPU可访问，性能最优但CPU无法直接访问
    CPU_Visible,       ///< CPU可访问，但可能需要手动同步
    GPU_CPU_Coherent,  ///< GPU和CPU都可访问，且自动同步（推荐用于频繁更新的数据）
};

/// 图元拓扑枚举，定义了顶点数据如何组装成图元
enum class PrimitiveTopology : uint8_t {
    PointList,     ///< 点列表
    LineList,      ///< 线列表
    LineStrip,     ///< 线带
    LineLoop,      ///< 环线
    TriangleList,  ///< 三角形列表
    TriangleStrip, ///< 三角形带
    TriangleFan,   ///< 三角形扇
};

/// 深度/模板比较函数枚举
enum class CompareFunc : uint8_t {
    Never,      ///< 永不通过
    Less,       ///< 小于时通过
    Equal,      ///< 等于时通过
    LessEqual,  ///< 小于等于时通过
    Greater,    ///< 大于时通过
    Always,     ///< 始终通过
};

/// 混合因子枚举，用于指定颜色混合的源和目标因子
enum class BlendFactor : uint8_t {
    Zero,              ///< (0, 0, 0, 0)
    One,               ///< (1, 1, 1, 1)
    SrcAlpha,          ///< (src.a, src.a, src.a, src.a)
    OneMinusSrcAlpha,  ///< (1-src.a, 1-src.a, 1-src.a, 1-src.a)
};

/// 混合操作枚举
enum class BlendOp : uint8_t {
    Add,  ///< 加法混合：result = src * srcFactor + dst * dstFactor
};

/// 顶点格式枚举，定义了顶点数据的布局方式
enum class VertexFormat : uint8_t {
    P3C3,    ///< 3D位置 + 3通道颜色 (24字节)
    P3C4,    ///< 3D位置 + 4通道颜色 (28字节)
    P3N3,    ///< 3D位置 + 3通道法向量 (24字节)
    P3T2,    ///< 3D位置 + 2通道纹理坐标 (20字节)
    P3T2C4,  ///< 3D位置 + 2通道纹理坐标 + 4通道颜色 (36字节)
};

/**
 * @brief 缓冲区创建描述结构
 */
struct BufferDesc {
    uint64_t    size;       ///< 缓冲区大小（字节）
    BufferUsage usage;      ///< 缓冲区用途
    MemoryType  memory;     ///< 内存类型
    const char* debugName;  ///< 调试名称（可选）
};

/**
 * @brief 纹理创建描述结构
 */
struct TextureDesc {
    uint32_t width;        ///< 纹理宽度（像素）
    uint32_t height;       ///< 纹理高度（像素）
    Format   format;       ///< 纹理格式
    uint32_t mipLevels;    ///< MIP级别数量
    const char* debugName; ///< 调试名称（可选）
};

/**
 * @brief 图形管线创建描述结构
 */
struct PipelineDesc {
    PrimitiveTopology topology;      ///< 图元拓扑类型
    const char*  vertexShader;      ///< 顶点着色器源码
    const char*  fragmentShader;    ///< 片段着色器源码
    const char*  computeShader;     ///< 计算着色器源码（可选）
    VertexFormat vertexFormat;      ///< 顶点格式
    bool         depthTest;         ///< 是否启用深度测试
    bool         depthWrite;        ///< 是否启用深度写入
    bool         blendEnable;       ///< 是否启用混合
    BlendFactor  srcBlend;          ///< 源混合因子
    BlendFactor  dstBlend;          ///< 目标混合因子
    CompareFunc  depthFunc;         ///< 深度比较函数
};

/// 缓冲区句柄类型
using BufferHandle   = uint64_t;
/// 纹理句柄类型
using TextureHandle  = uint64_t;
/// 管线句柄类型
using PipelineHandle = uint64_t;

/// 无效句柄常量
constexpr uint64_t NullHandle = 0;

/**
 * @brief 视口结构
 */
struct Viewport {
    float x, y;  ///< 视口左上角坐标
    float w, h;  ///< 视口宽度和高度
};

/**
 * @brief 裁剪矩形结构
 */
struct Scissor {
    int32_t x, y;      ///< 裁剪矩形左上角坐标
    uint32_t w, h;     ///< 裁剪矩形宽度和高度
};

/**
 * @brief 顶点属性描述结构
 */
struct VertexAttrib {
    uint32_t location;    ///< 属性位置（对应着色器中的layout(location=N)）
    uint32_t offset;      ///< 在顶点结构中的偏移量（字节）
    uint32_t stride;      ///< 顶点步长（字节）
    bool     normalized;  ///< 是否需要归一化
};

}
