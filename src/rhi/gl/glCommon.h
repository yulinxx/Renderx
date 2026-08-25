/**
 * @file glCommon.h
 * @brief GL 后端内部：RHI 枚举 → GL 枚举 的集中映射，以及资源记录定义
 *
 * 集中放这里的理由：旧 rhiGl.cpp 把这些 switch 散落在 2000 多行里，
 * 同一个映射（如 BlendFactor）出现多份且互不一致。本文件是 GL 后端
 * 唯一的枚举翻译层，新增格式只改这里。
 *
 * 本文件不包含任何 RHI 之外的头文件，也不引用公共 ABI。
 */
#pragma once

#include "platform/glLoader.h"
#include "rhi/rhiCore.h"

#include <vector>

// ==================== 常量兜底 ====================
//
// Windows 只提供 GL 1.1 的 <GL/gl.h>，macOS 的 <OpenGL/gl3.h> 到 4.1，
// 二者都缺一部分本文件用到的枚举。全部按 #ifndef 兜底，数值取自
// OpenGL registry，与 glLoader.h 中已有的兜底风格一致。

#ifndef GL_ZERO
    #define GL_ZERO 0
#endif
#ifndef GL_ONE
    #define GL_ONE 1
#endif
#ifndef GL_SRC_COLOR
    #define GL_SRC_COLOR 0x0300
#endif
#ifndef GL_ONE_MINUS_SRC_COLOR
    #define GL_ONE_MINUS_SRC_COLOR 0x0301
#endif
#ifndef GL_SRC_ALPHA
    #define GL_SRC_ALPHA 0x0302
#endif
#ifndef GL_ONE_MINUS_SRC_ALPHA
    #define GL_ONE_MINUS_SRC_ALPHA 0x0303
#endif
#ifndef GL_DST_ALPHA
    #define GL_DST_ALPHA 0x0304
#endif
#ifndef GL_ONE_MINUS_DST_ALPHA
    #define GL_ONE_MINUS_DST_ALPHA 0x0305
#endif

#ifndef GL_NEVER
    #define GL_NEVER 0x0200
#endif
#ifndef GL_LESS
    #define GL_LESS 0x0201
#endif
#ifndef GL_EQUAL
    #define GL_EQUAL 0x0202
#endif
#ifndef GL_GREATER
    #define GL_GREATER 0x0204
#endif
#ifndef GL_NOTEQUAL
    #define GL_NOTEQUAL 0x0205
#endif
#ifndef GL_GEQUAL
    #define GL_GEQUAL 0x0206
#endif
#ifndef GL_ALWAYS
    #define GL_ALWAYS 0x0207
#endif

#ifndef GL_NEAREST
    #define GL_NEAREST 0x2600
#endif
#ifndef GL_LINEAR
    #define GL_LINEAR 0x2601
#endif
#ifndef GL_REPEAT
    #define GL_REPEAT 0x2901
#endif
#ifndef GL_MIRRORED_REPEAT
    #define GL_MIRRORED_REPEAT 0x8370
#endif
#ifndef GL_CLAMP_TO_BORDER
    #define GL_CLAMP_TO_BORDER 0x812D
#endif
// 点大小由顶点着色器的 gl_PointSize 决定。桌面 GL 核心 profile 下必须显式
// 开启此开关，否则只使用 glPointSize() 的全局值，shader 里写的值被静默忽略。
// GL ES 无此枚举——ES 上点大小恒由着色器决定。
#ifndef GL_PROGRAM_POINT_SIZE
    #define GL_PROGRAM_POINT_SIZE 0x8642
#endif

#ifndef GL_SRGB8_ALPHA8
    #define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_R16F
    #define GL_R16F 0x822D
#endif
#ifndef GL_RG16F
    #define GL_RG16F 0x822F
#endif
#ifndef GL_RGBA16F
    #define GL_RGBA16F 0x881A
#endif
#ifndef GL_R32F
    #define GL_R32F 0x822E
#endif
#ifndef GL_RG32F
    #define GL_RG32F 0x8230
#endif
#ifndef GL_RGBA32F
    #define GL_RGBA32F 0x8814
#endif
#ifndef GL_R32UI
    #define GL_R32UI 0x8236
#endif
#ifndef GL_DEPTH32F_STENCIL8
    #define GL_DEPTH32F_STENCIL8 0x8CAD
#endif
#ifndef GL_RED_INTEGER
    #define GL_RED_INTEGER 0x8D94
#endif
#ifndef GL_BGRA
    #define GL_BGRA 0x80E1
#endif
#ifndef GL_DEPTH_COMPONENT
    #define GL_DEPTH_COMPONENT 0x1902
#endif
#ifndef GL_DEPTH_STENCIL
    #define GL_DEPTH_STENCIL 0x84F9
#endif
#ifndef GL_UNSIGNED_INT_24_8
    #define GL_UNSIGNED_INT_24_8 0x84FA
#endif
#ifndef GL_FLOAT_32_UNSIGNED_INT_24_8_REV
    #define GL_FLOAT_32_UNSIGNED_INT_24_8_REV 0x8DAD
#endif
#ifndef GL_HALF_FLOAT
    #define GL_HALF_FLOAT 0x140B
#endif

#ifndef GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT
    #define GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT 0x00000001
#endif
#ifndef GL_ELEMENT_ARRAY_BARRIER_BIT
    #define GL_ELEMENT_ARRAY_BARRIER_BIT 0x00000002
#endif
#ifndef GL_UNIFORM_BARRIER_BIT
    #define GL_UNIFORM_BARRIER_BIT 0x00000004
#endif
#ifndef GL_TEXTURE_FETCH_BARRIER_BIT
    #define GL_TEXTURE_FETCH_BARRIER_BIT 0x00000008
#endif
#ifndef GL_FRAMEBUFFER_BARRIER_BIT
    #define GL_FRAMEBUFFER_BARRIER_BIT 0x00000400
#endif
#ifndef GL_ALL_BARRIER_BITS
    #define GL_ALL_BARRIER_BITS 0xFFFFFFFF
#endif

#ifndef GL_TEXTURE0
    #define GL_TEXTURE0 0x84C0
#endif
#ifndef GL_TEXTURE_MIN_FILTER
    #define GL_TEXTURE_MIN_FILTER 0x2801
#endif
#ifndef GL_TEXTURE_MAG_FILTER
    #define GL_TEXTURE_MAG_FILTER 0x2800
#endif
#ifndef GL_TEXTURE_WRAP_S
    #define GL_TEXTURE_WRAP_S 0x2802
#endif
#ifndef GL_TEXTURE_WRAP_T
    #define GL_TEXTURE_WRAP_T 0x2803
#endif
#ifndef GL_FRONT_AND_BACK
    #define GL_FRONT_AND_BACK 0x0408
#endif

namespace Render::RHI::gl
{

    // ==================== 枚举映射 ====================

    inline GLenum toGlTopology(PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::PointList: return GL_POINTS;
        case PrimitiveTopology::LineList: return GL_LINES;
        case PrimitiveTopology::LineStrip: return GL_LINE_STRIP;
        case PrimitiveTopology::TriangleList: return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        }
        return GL_TRIANGLES;
    }

    inline GLenum toGlCompareOp(CompareOp op)
    {
        switch (op)
        {
        case CompareOp::Never: return GL_NEVER;
        case CompareOp::Less: return GL_LESS;
        case CompareOp::Equal: return GL_EQUAL;
        case CompareOp::LessEqual: return GL_LEQUAL;
        case CompareOp::Greater: return GL_GREATER;
        case CompareOp::NotEqual: return GL_NOTEQUAL;
        case CompareOp::GreaterEqual: return GL_GEQUAL;
        case CompareOp::Always: return GL_ALWAYS;
        }
        return GL_LEQUAL;
    }

    inline GLenum toGlBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return GL_ZERO;
        case BlendFactor::One: return GL_ONE;
        case BlendFactor::SrcColor: return GL_SRC_COLOR;
        case BlendFactor::OneMinusSrcColor: return GL_ONE_MINUS_SRC_COLOR;
        case BlendFactor::SrcAlpha: return GL_SRC_ALPHA;
        case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
        case BlendFactor::DstAlpha: return GL_DST_ALPHA;
        case BlendFactor::OneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
        }
        return GL_ONE;
    }

    inline GLenum toGlBlendOp(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add: return GL_FUNC_ADD;
        case BlendOp::Subtract: return GL_FUNC_SUBTRACT;
        case BlendOp::ReverseSubtract: return GL_FUNC_REVERSE_SUBTRACT;
        case BlendOp::Min: return GL_MIN;
        case BlendOp::Max: return GL_MAX;
        }
        return GL_FUNC_ADD;
    }

    /// 纹理内部格式（sized internal format，GL 3.3 core 要求显式给定）
    inline GLenum toGlInternalFormat(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm: return GL_R8;
        case Format::RG8Unorm: return GL_RG8;
        case Format::RGBA8Unorm:
        // GL 没有 BGRA 内部格式：BGRA 只是上传时的通道顺序，
        // 内部一律存 RGBA8。交换链格式选 BGRA8 时在此归一。
        case Format::BGRA8Unorm: return GL_RGBA8;
        case Format::RGBA8Srgb:
        case Format::BGRA8Srgb: return GL_SRGB8_ALPHA8;
        case Format::R16Float: return GL_R16F;
        case Format::RG16Float: return GL_RG16F;
        case Format::RGBA16Float: return GL_RGBA16F;
        case Format::R32Float: return GL_R32F;
        case Format::RG32Float: return GL_RG32F;
        case Format::RGBA32Float: return GL_RGBA32F;
        case Format::R32Uint: return GL_R32UI;
        case Format::D32Float: return GL_DEPTH_COMPONENT32F;
        case Format::D24UnormS8Uint: return GL_DEPTH24_STENCIL8;
        case Format::D32FloatS8Uint: return GL_DEPTH32F_STENCIL8;
        case Format::Unknown: return 0;
        }
        return 0;
    }

    /// 上传/读回时的通道顺序
    inline GLenum toGlBaseFormat(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm:
        case Format::R16Float:
        case Format::R32Float: return GL_RED;
        case Format::R32Uint: return GL_RED_INTEGER;
        case Format::RG8Unorm:
        case Format::RG16Float:
        case Format::RG32Float: return GL_RG;
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::RGBA16Float:
        case Format::RGBA32Float: return GL_RGBA;
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb: return GL_BGRA;
        case Format::D32Float: return GL_DEPTH_COMPONENT;
        case Format::D24UnormS8Uint:
        case Format::D32FloatS8Uint: return GL_DEPTH_STENCIL;
        case Format::Unknown: return 0;
        }
        return 0;
    }

    inline GLenum toGlPixelType(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm:
        case Format::RG8Unorm:
        case Format::RGBA8Unorm:
        case Format::RGBA8Srgb:
        case Format::BGRA8Unorm:
        case Format::BGRA8Srgb: return GL_UNSIGNED_BYTE;
        case Format::R16Float:
        case Format::RG16Float:
        case Format::RGBA16Float: return GL_HALF_FLOAT;
        case Format::R32Float:
        case Format::RG32Float:
        case Format::RGBA32Float:
        case Format::D32Float: return GL_FLOAT;
        case Format::R32Uint: return GL_UNSIGNED_INT;
        case Format::D24UnormS8Uint: return GL_UNSIGNED_INT_24_8;
        case Format::D32FloatS8Uint: return GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
        case Format::Unknown: return 0;
        }
        return GL_UNSIGNED_BYTE;
    }

    /// 顶点属性类型 → (GL 基础类型, 分量数, 是否归一化)
    struct GlVertexAttribFormat
    {
        GLenum type = GL_FLOAT;
        GLint components = 3;
        GLboolean normalized = GL_FALSE;
        bool isInteger = false;
    };

    inline GlVertexAttribFormat toGlVertexAttrib(VertexAttribType type)
    {
        switch (type)
        {
        case VertexAttribType::Float1: return { GL_FLOAT, 1, GL_FALSE, false };
        case VertexAttribType::Float2: return { GL_FLOAT, 2, GL_FALSE, false };
        case VertexAttribType::Float3: return { GL_FLOAT, 3, GL_FALSE, false };
        case VertexAttribType::Float4: return { GL_FLOAT, 4, GL_FALSE, false };
        case VertexAttribType::Uint8x4Norm: return { GL_UNSIGNED_BYTE, 4, GL_TRUE, false };
        case VertexAttribType::Uint32x1: return { GL_UNSIGNED_INT, 1, GL_FALSE, true };
        }
        return { GL_FLOAT, 3, GL_FALSE, false };
    }

    /**
     * @brief 缓冲区主用途 → GL 绑定目标
     *
     * GL 的 target 只影响绑定点，不影响存储，因此取第一个匹配用途即可。
     */
    inline GLenum toGlBufferTarget(BufferUsage usage)
    {
        if (hasFlag(usage, BufferUsage::Index)) { return GL_ELEMENT_ARRAY_BUFFER; }
        if (hasFlag(usage, BufferUsage::Vertex)) { return GL_ARRAY_BUFFER; }
        if (hasFlag(usage, BufferUsage::Uniform)) { return GL_UNIFORM_BUFFER; }
        if (hasFlag(usage, BufferUsage::Storage)) { return GL_SHADER_STORAGE_BUFFER; }
        if (hasFlag(usage, BufferUsage::Indirect)) { return GL_DRAW_INDIRECT_BUFFER; }
        return GL_ARRAY_BUFFER;
    }

    inline GLenum toGlBufferUsageHint(MemoryAccess access)
    {
        switch (access)
        {
        case MemoryAccess::GpuOnly: return GL_STATIC_DRAW;
        case MemoryAccess::CpuToGpu:
        case MemoryAccess::CpuToGpuCoherent: return GL_STREAM_DRAW;
        case MemoryAccess::GpuToCpu: return GL_DYNAMIC_DRAW;
        }
        return GL_STATIC_DRAW;
    }

    /**
     * @brief BarrierScope → glMemoryBarrier 位
     *
     * 旧版 BarrierFlag 直接把 GL 的位值写进 RHI 公共枚举，
     * 于是 Vulkan/Metal 必须反向翻译 GL 语义。现在翻译只发生在这里。
     */
    inline GLbitfield toGlBarrierBits(BarrierScope scope)
    {
        if (scope == BarrierScope::All)
        {
            return GL_ALL_BARRIER_BITS;
        }
        GLbitfield bits = 0;
        if (hasFlag(scope, BarrierScope::VertexInput)) { bits |= GL_VERTEX_ATTRIB_ARRAY_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::IndexInput)) { bits |= GL_ELEMENT_ARRAY_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::UniformRead)) { bits |= GL_UNIFORM_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::TextureRead)) { bits |= GL_TEXTURE_FETCH_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::StorageReadWrite)) { bits |= GL_SHADER_STORAGE_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::IndirectCommandRead)) { bits |= GL_COMMAND_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::RenderTargetWrite)) { bits |= GL_FRAMEBUFFER_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::TransferReadWrite)) { bits |= GL_BUFFER_UPDATE_BARRIER_BIT; }
        if (hasFlag(scope, BarrierScope::HostRead)) { bits |= GL_BUFFER_UPDATE_BARRIER_BIT; }
        return bits;
    }

    inline GLenum toGlFilter(FilterMode filter)
    {
        return filter == FilterMode::Nearest ? GL_NEAREST : GL_LINEAR;
    }

    inline GLenum toGlAddressMode(AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat: return GL_REPEAT;
        case AddressMode::MirrorRepeat: return GL_MIRRORED_REPEAT;
        case AddressMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case AddressMode::ClampToBorder: return GL_CLAMP_TO_BORDER;
        }
        return GL_CLAMP_TO_EDGE;
    }

    // ==================== 资源记录 ====================

    struct GlBufferRecord
    {
        GLuint name = 0;
        BufferDesc desc{};
        GLenum target = GL_ARRAY_BUFFER;
        void* mappedPtr = nullptr;
    };

    struct GlTextureRecord
    {
        GLuint name = 0;
        TextureDesc desc{};
    };

    struct GlSamplerRecord
    {
        SamplerDesc desc{};
    };

    /**
     * @brief 着色器记录
     *
     * 存的是**源码副本**而非已编译的 GL shader 对象：ShaderDesc 里没有
     * stage 字段（SPIR-V / metallib 自带入口阶段信息，Vulkan/Metal 不需要），
     * 而 glCreateShader 必须在创建时就给出 GL_VERTEX_SHADER / GL_FRAGMENT_SHADER。
     * 因此 GL 后端把编译推迟到 createGraphicsPipeline / createComputePipeline，
     * 那里才知道每个句柄承担哪个阶段。
     */
    struct GlShaderRecord
    {
        ShaderLanguage language = ShaderLanguage::GlslSource;
        std::vector<char> source;  ///< NUL 结尾
    };

    /// 一个 (set, binding) 槽位在 GL 侧的落点
    struct GlBindingMapping
    {
        uint32_t set = 0;
        uint32_t binding = 0;
        BindingType type = BindingType::UniformBuffer;
        /// UniformBuffer/StorageBuffer：UBO/SSBO binding point
        /// SampledTexture/StorageTexture：纹理单元编号
        uint32_t glSlot = 0;
    };

    struct GlPipelineRecord
    {
        GLuint program = 0;
        GLuint vao = 0;
        bool isCompute = false;

        GLenum topology = GL_TRIANGLES;
        VertexAttribute attributes[kMaxVertexAttributes]{};
        uint32_t attributeCount = 0;
        VertexBufferLayout bufferLayouts[kMaxVertexBufferSlots]{};
        uint32_t bufferLayoutCount = 0;

        RasterState raster{};
        DepthStencilState depthStencil{};
        ColorBlendState blend{};

        uint32_t pushConstantBytes = 0;
        std::vector<GlBindingMapping> bindings;
    };

    struct GlBindGroupRecord
    {
        std::vector<BufferBinding> buffers;
        std::vector<TextureBinding> textures;
    };

}  // namespace Render::RHI::gl
