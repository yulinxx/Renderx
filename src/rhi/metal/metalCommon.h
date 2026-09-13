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
     * 与 GL 侧同样保存字节副本而非已编译对象：ShaderDesc 没有 stage 字段
     * （metallib 自带函数名），而 MTLFunction 必须按名字从 library 里取，
     * 「取哪个函数」要到 createGraphicsPipeline / createComputePipeline 才知道。
     */
    struct MetalShaderRecord
    {
        ShaderLanguage language = ShaderLanguage::MetalLib;
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
    /// Metal 的 buffer/texture 绑定表是一维的，set 只是 RHI 层的分组语义，
    /// 这里按 set * kMaxBindingsPerSet + binding 展开，保证同一个 set 内不冲突。
    inline uint32_t toMetalBindingIndex(uint32_t set, uint32_t binding)
    {
        return set * kMaxBindingsPerSet + binding;
    }

}  // namespace Render::RHI::metal
