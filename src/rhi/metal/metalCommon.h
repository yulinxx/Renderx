/**
 * @file metalCommon.h
 * @brief Metal 后端：资源记录与枚举转换（仅 Apple 平台编译）
 *
 * 与 glCommon.h 的分工一致：本文件只放「后端私有的资源记录 + RHI 枚举到
 * 原生枚举的转换」，供 metalDevice.mm / metalCommandList.mm 共用。
 *
 * ObjC 对象由 ARC 管理：.mm 以 -fobjc-arc 编译，记录里的 id<MTL*> 成员随
 * 记录析构自动释放。因此 destroy* 只需把记录从池里摘除，不写额外清理——
 * 手写 release 与 ARC 混用是崩溃与泄漏的常见来源。
 */
#pragma once

#import <Metal/Metal.h>

#include "rhi/rhiCore.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Render::RHI::metal
{
    struct MetalBufferRecord
    {
        id<MTLBuffer> buffer = nil;
        BufferDesc desc{};
    };

    struct MetalTextureRecord
    {
        id<MTLTexture> texture = nil;
        TextureDesc desc{};
    };

    struct MetalSamplerRecord
    {
        id<MTLSamplerState> sampler = nil;
        SamplerDesc desc{};
    };

    /**
     * @brief 着色器记录
     *
     * 与 GL 侧同样保存字节副本而非已编译对象：MTLLibrary 是**按文件**建立的
     * 容器（一份 metallib 里可以有多个函数），而「这个句柄当顶点还是片段用」
     * 只有调用方知道，因此入口函数名随句柄一起存，创建管线时按它取 MTLFunction。
     */
    struct MetalShaderRecord
    {
        ShaderLanguage language = ShaderLanguage::MetalLib;
        /// 入口函数名。MSL 的顶点/片段函数不能都叫 main，故约定为
        /// vs_main / fs_main（见 src/shader/metal/ 下的 shader 与构建链）。
        std::string entryPoint = "main";
        std::vector<uint8_t> bytes;
    };

    /**
     * @brief 绑定组记录
     *
     * Metal 不需要把 (set, binding) 预解析成名字对应的槽位：
     * 绑定就是 setBuffer/setTexture 按 index 下发，因此这里只存资源引用。
     */
    struct MetalBindGroupRecord
    {
        std::vector<BufferBinding> buffers;
        std::vector<TextureBinding> textures;
    };

    // ==================== 枚举转换 ====================

    inline MTLPixelFormat toMetalFormat(Format format)
    {
        switch (format)
        {
        case Format::R8Unorm: return MTLPixelFormatR8Unorm;
        case Format::RG8Unorm: return MTLPixelFormatRG8Unorm;
        case Format::RGBA8Unorm: return MTLPixelFormatRGBA8Unorm;
        case Format::RGBA8Srgb: return MTLPixelFormatRGBA8Unorm_sRGB;
        case Format::BGRA8Unorm: return MTLPixelFormatBGRA8Unorm;
        case Format::BGRA8Srgb: return MTLPixelFormatBGRA8Unorm_sRGB;
        case Format::R16Float: return MTLPixelFormatR16Float;
        case Format::RG16Float: return MTLPixelFormatRG16Float;
        case Format::RGBA16Float: return MTLPixelFormatRGBA16Float;
        case Format::R32Float: return MTLPixelFormatR32Float;
        case Format::RG32Float: return MTLPixelFormatRG32Float;
        case Format::RGBA32Float: return MTLPixelFormatRGBA32Float;
        case Format::R32Uint: return MTLPixelFormatR32Uint;
        case Format::D32Float: return MTLPixelFormatDepth32Float;
        case Format::D24UnormS8Uint: return MTLPixelFormatDepth24Unorm_Stencil8;
        case Format::D32FloatS8Uint: return MTLPixelFormatDepth32Float_Stencil8;
        case Format::Unknown: return MTLPixelFormatInvalid;
        }
        return MTLPixelFormatInvalid;
    }

    inline Format fromMetalFormat(MTLPixelFormat format)
    {
        switch (format)
        {
        case MTLPixelFormatR8Unorm: return Format::R8Unorm;
        case MTLPixelFormatRG8Unorm: return Format::RG8Unorm;
        case MTLPixelFormatRGBA8Unorm: return Format::RGBA8Unorm;
        case MTLPixelFormatRGBA8Unorm_sRGB: return Format::RGBA8Srgb;
        case MTLPixelFormatBGRA8Unorm: return Format::BGRA8Unorm;
        case MTLPixelFormatBGRA8Unorm_sRGB: return Format::BGRA8Srgb;
        case MTLPixelFormatDepth32Float: return Format::D32Float;
        case MTLPixelFormatDepth24Unorm_Stencil8: return Format::D24UnormS8Uint;
        case MTLPixelFormatDepth32Float_Stencil8: return Format::D32FloatS8Uint;
        default: return Format::Unknown;
        }
    }

    inline MTLSamplerMinMagFilter toMetalFilter(FilterMode mode)
    {
        return mode == FilterMode::Nearest ? MTLSamplerMinMagFilterNearest
                                           : MTLSamplerMinMagFilterLinear;
    }

    inline MTLSamplerAddressMode toMetalAddressMode(AddressMode mode)
    {
        switch (mode)
        {
        case AddressMode::Repeat: return MTLSamplerAddressModeRepeat;
        case AddressMode::MirrorRepeat: return MTLSamplerAddressModeMirrorRepeat;
        case AddressMode::ClampToEdge: return MTLSamplerAddressModeClampToEdge;
        case AddressMode::ClampToBorder: return MTLSamplerAddressModeClampToBorderColor;
        }
        return MTLSamplerAddressModeClampToEdge;
    }

    /// (set, binding) 到 Metal 的扁平 index。
    /// Metal 的 buffer/texture 绑定表是一维的，set 只是 RHI 层的分组语义。
    inline uint32_t toMetalBindingIndex(uint32_t set, uint32_t binding)
    {
        return set * kMaxBindingsPerSet + binding;
    }

    // ==================== 管线状态转换 ====================

    inline MTLPrimitiveType toMetalTopology(PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::PointList: return MTLPrimitiveTypePoint;
        case PrimitiveTopology::LineList: return MTLPrimitiveTypeLine;
        case PrimitiveTopology::LineStrip: return MTLPrimitiveTypeLineStrip;
        case PrimitiveTopology::TriangleList: return MTLPrimitiveTypeTriangle;
        case PrimitiveTopology::TriangleStrip: return MTLPrimitiveTypeTriangleStrip;
        }
        return MTLPrimitiveTypeTriangle;
    }

    /// VertexAttribType → MTLVertexFormat。返回 Invalid 表示 Metal 侧无对应物。
    inline MTLVertexFormat toMetalVertexFormat(VertexAttribType type)
    {
        switch (type)
        {
        case VertexAttribType::Float1: return MTLVertexFormatFloat;
        case VertexAttribType::Float2: return MTLVertexFormatFloat2;
        case VertexAttribType::Float3: return MTLVertexFormatFloat3;
        case VertexAttribType::Float4: return MTLVertexFormatFloat4;
        case VertexAttribType::Uint8x4Norm: return MTLVertexFormatUChar4Normalized;
        case VertexAttribType::Uint32x1: return MTLVertexFormatUInt;
        }
        return MTLVertexFormatInvalid;
    }

    inline MTLCompareFunction toMetalCompareOp(CompareOp op)
    {
        switch (op)
        {
        case CompareOp::Never: return MTLCompareFunctionNever;
        case CompareOp::Less: return MTLCompareFunctionLess;
        case CompareOp::Equal: return MTLCompareFunctionEqual;
        case CompareOp::LessEqual: return MTLCompareFunctionLessEqual;
        case CompareOp::Greater: return MTLCompareFunctionGreater;
        case CompareOp::NotEqual: return MTLCompareFunctionNotEqual;
        case CompareOp::GreaterEqual: return MTLCompareFunctionGreaterEqual;
        case CompareOp::Always: return MTLCompareFunctionAlways;
        }
        return MTLCompareFunctionAlways;
    }

    inline MTLBlendFactor toMetalBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return MTLBlendFactorZero;
        case BlendFactor::One: return MTLBlendFactorOne;
        case BlendFactor::SrcColor: return MTLBlendFactorSourceColor;
        case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
        case BlendFactor::SrcAlpha: return MTLBlendFactorSourceAlpha;
        case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
        case BlendFactor::DstAlpha: return MTLBlendFactorDestinationAlpha;
        case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
        }
        return MTLBlendFactorOne;
    }

    inline MTLBlendOperation toMetalBlendOp(BlendOp op)
    {
        switch (op)
        {
        case BlendOp::Add: return MTLBlendOperationAdd;
        case BlendOp::Subtract: return MTLBlendOperationSubtract;
        case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
        case BlendOp::Min: return MTLBlendOperationMin;
        case BlendOp::Max: return MTLBlendOperationMax;
        }
        return MTLBlendOperationAdd;
    }

    inline MTLCullMode toMetalCullMode(CullMode mode)
    {
        switch (mode)
        {
        case CullMode::None: return MTLCullModeNone;
        case CullMode::Front: return MTLCullModeFront;
        case CullMode::Back: return MTLCullModeBack;
        }
        return MTLCullModeNone;
    }

    inline MTLWinding toMetalWinding(FrontFace face)
    {
        return face == FrontFace::Clockwise ? MTLWindingClockwise : MTLWindingCounterClockwise;
    }

    // ==================== 资源槽位约定 ====================
    //
    // Metal 的「参数表」每个 stage 一张、是一维的：顶点缓冲与 buffer 绑定共享
    // 同一张表（MSL 里的 [[buffer(n)]]），纹理另有一张（[[texture(n)]]）。
    // 因此**不能**把 (set, binding) 直接当 buffer index —— 顶点缓冲 slot 0..3
    // 会与 set=0 的 binding 0..3 撞车，表现为「顶点数据被当成 uniform 读」。
    //
    // 本后端统一约定，MSL 侧必须按同一套 index 声明：
    //   buffer 表   0..3    顶点缓冲（= VertexBufferLayout::slot）
    //               16..29  bindGroup 的 buffer（kMetalBindGroupBufferBase + set*16 + binding）
    //               30      pushConstant 块（kMetalPushConstantIndex）
    //   texture 表  0..15   bindGroup 的纹理（set*16 + binding）

    constexpr uint32_t kMetalPushConstantIndex = 30;
    constexpr uint32_t kMetalBindGroupBufferBase = 16;

    /// bindGroup 的 buffer 落到 Metal buffer 表的 index
    inline uint32_t toMetalBufferIndex(uint32_t set, uint32_t binding)
    {
        return kMetalBindGroupBufferBase + toMetalBindingIndex(set, binding);
    }

    /// bindGroup 的纹理落到 Metal texture 表的 index
    inline uint32_t toMetalTextureIndex(uint32_t set, uint32_t binding)
    {
        return toMetalBindingIndex(set, binding);
    }

}  // namespace Render::RHI::metal
