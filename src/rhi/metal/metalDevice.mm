/**
 * @file metalDevice.mm
 * @brief Metal 后端：设备与表面的实现（仅 Apple 平台编译）
 *
 * 本文件只覆盖 M1 范围：设备、表面、交换链、纹理/缓冲/采样器/绑定组的创建，
 * 以及读回。管线与绘制（M2）、compute 与 indirect（M3）之后补齐——
 * 未实现的入口一律记录 error 并返回无效值，不做静默空转。
 */

#import <AppKit/AppKit.h>

#include "rhi/metal/metalDevice.h"

#include <cstdio>
#include <cstring>

namespace Render::RHI::metal
{

    namespace
    {
        /// 可被读回的暂存缓冲行距必须按 256 对齐（Metal 对 blit 的硬性要求）
        constexpr uint32_t kReadbackPitchAlignment = 256;

        uint32_t alignUp(uint32_t value, uint32_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        MTLResourceOptions toResourceOptions(MemoryAccess access)
        {
            // M1 统一用 Shared：macOS 的 Apple Silicon 是统一内存，GPU 访问
            // Shared 与 Private 的开销接近，而 Shared 允许 writeBuffer 直接
            // 写 contents，省掉一整条 staging + blit 路径。
            // 后续若要压低显存带宽，再把 GpuOnly 改为 Private + blit 上传。
            (void)access;
            return MTLResourceStorageModeShared;
        }
    }  // namespace

    // ======================================================================
    // MetalSurface
    // ======================================================================

    MetalSurface::MetalSurface(MetalDevice* device, const SurfaceDesc& desc, const RhiLogger& logger)
        : m_device(device)
        , m_window(desc.window)
        , m_extent(desc.initialExtent)
        , m_colorFormat(desc.preferredColorFormat)
        , m_depthFormat(desc.depthFormat)
        , m_presentMode(desc.presentMode)
        , m_log(logger)
    {
        m_inFlight = dispatch_semaphore_create(3);

        NSView* view = (__bridge NSView*)desc.window.handleA;
        if (view == nil)
        {
            m_log.error("[metal] createSurface: CocoaNsView 的 handleA 为空");
            return;
        }

        m_layer = [CAMetalLayer layer];
        m_layer.device = m_device->nativeDevice();
        m_layer.pixelFormat = toMetalFormat(m_colorFormat);
        m_layer.framebufferOnly = NO;  // 视图导出需要从后备缓冲读回
        m_layer.opaque = YES;

        // 像素尺寸由调用方给出（与 GL 侧的 framebuffer 尺寸同一约定），
        // contentsScale 只影响 layer 把像素映射到点的方式。
        const CGFloat scale = view.window != nil ? view.window.backingScaleFactor : 1.0;
        m_layer.contentsScale = scale > 0.0 ? scale : 1.0;
        m_layer.drawableSize = CGSizeMake(m_extent.width, m_extent.height);

        view.wantsLayer = YES;
        view.layer = m_layer;

        ensureDepthTexture();
    }

    MetalSurface::~MetalSurface()
    {
        releaseDrawable();
        m_depthNative = nil;
        if (m_layer != nil)
        {
            // 摘掉宿主视图上的 layer，避免留下一个指向已销毁后端的悬垂层
            NSView* view = (__bridge NSView*)m_window.handleA;
            if (view != nil && view.layer == m_layer)
            {
                view.layer = nil;
            }
            m_layer = nil;
        }
        // 表面托管的纹理记录要随表面一起摘除，否则纹理会一直挂在设备池里
        if (m_device != nullptr)
        {
            if (m_colorTexture.valid())
            {
                m_device->destroyTexture(m_colorTexture);
            }
            if (m_depthTexture.valid())
            {
                m_device->destroyTexture(m_depthTexture);
            }
        }
        m_inFlight = nil;
    }

    void MetalSurface::ensureDepthTexture()
    {
        m_depthNative = nil;
        if (m_depthFormat == Format::Unknown || m_extent.width == 0 || m_extent.height == 0)
        {
            return;
        }

        MTLTextureDescriptor* desc = [MTLTextureDescriptor new];
        desc.textureType = MTLTextureType2D;
        desc.pixelFormat = toMetalFormat(m_depthFormat);
        desc.width = m_extent.width;
        desc.height = m_extent.height;
        desc.usage = MTLTextureUsageRenderTarget;
        desc.storageMode = MTLResourceStorageModePrivate;

        m_depthNative = [m_device->nativeDevice() newTextureWithDescriptor:desc];
        if (m_depthNative == nil)
        {
            m_log.error("[metal] 深度纹理创建失败（%ux%u）", m_extent.width, m_extent.height);
            return;
        }

        TextureDesc depthDesc{};
        depthDesc.width = m_extent.width;
        depthDesc.height = m_extent.height;
        depthDesc.format = m_depthFormat;
        depthDesc.usage = TextureUsage::DepthStencilAttachment;
        m_depthTexture = m_device->updateBoundTexture(m_depthTexture, m_depthNative, depthDesc);
    }

    void MetalSurface::releaseDrawable()
    {
        m_drawable = nil;
        m_acquired = false;
    }

    RhiResult MetalSurface::acquireNextImage()
    {
        if (m_layer == nil)
        {
            m_log.error("[metal] acquireNextImage: 表面未正确初始化");
            return RhiResult::ErrorSurfaceLost;
        }
        // 尺寸为 0（窗口最小化）时安全跳过：此时既没有 drawable 也不必渲染
        if (m_extent.width == 0 || m_extent.height == 0)
        {
            releaseDrawable();
            return RhiResult::Ok;
        }
        if (m_acquired)
        {
            m_log.error("[metal] acquireNextImage: 本帧已获取过 drawable");
            return RhiResult::ErrorUnknown;
        }

        // 3 帧 in flight：等待最老的一帧完成，避免 CPU 无界地领先 GPU
        dispatch_semaphore_wait(m_inFlight, DISPATCH_TIME_FOREVER);

        m_drawable = [m_layer nextDrawable];
        if (m_drawable == nil)
        {
            dispatch_semaphore_signal(m_inFlight);
            m_log.error("[metal] acquireNextImage: nextDrawable 返回 nil");
            return RhiResult::ErrorSurfaceLost;
        }

        TextureDesc colorDesc{};
        colorDesc.width = static_cast<uint32_t>(m_drawable.texture.width);
        colorDesc.height = static_cast<uint32_t>(m_drawable.texture.height);
        colorDesc.format = fromMetalFormat(m_drawable.texture.pixelFormat);
        colorDesc.usage = TextureUsage::ColorAttachment | TextureUsage::TransferSrc;
        m_colorTexture = m_device->updateBoundTexture(m_colorTexture, m_drawable.texture, colorDesc);

        m_acquired = true;
        return RhiResult::Ok;
    }

    RhiResult MetalSurface::present()
    {
        if (m_layer == nil)
        {
            m_log.error("[metal] present: 表面未正确初始化");
            return RhiResult::ErrorSurfaceLost;
        }
        if (!m_acquired || m_drawable == nil)
        {
            // 最小化时 acquire 走的是「安全跳过」分支，present 同样应当是空操作
            return RhiResult::Ok;
        }

        id<MTLCommandBuffer> commandBuffer = m_device->commands().commandBuffer();
        if (commandBuffer == nil)
        {
            m_log.error("[metal] present: 本帧没有命令缓冲，beginFrame 未调用？");
            releaseDrawable();
            dispatch_semaphore_signal(m_inFlight);
            return RhiResult::ErrorInvalidHandle;
        }
        if (m_device->commands().inRenderPass())
        {
            m_log.warn("[metal] present: 仍有未结束的 RenderPass，已自动收尾");
            m_device->commands().endRenderPass();
        }

        // presentDrawable 必须在 commit 之前设置，因此 Metal 上「提交」与
        // 「呈现」无法像 GL 那样拆成两步：commit 与 present 在这里一起完成。
        // rxSession 的调用顺序（submitFrame 之后必然 present）保证了等价语义。
        dispatch_semaphore_t inFlight = m_inFlight;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
            (void)buffer;
            dispatch_semaphore_signal(inFlight);
        }];
        [commandBuffer presentDrawable:m_drawable];
        [commandBuffer commit];

        releaseDrawable();
        ++m_presentCount;
        return RhiResult::Ok;
    }

    RhiResult MetalSurface::resize(Extent2D extent)
    {
        // 尺寸为 0 表示窗口最小化：只记录，不重建附件
        m_extent = extent;
        if (m_layer != nil && extent.width != 0 && extent.height != 0)
        {
            m_layer.drawableSize = CGSizeMake(extent.width, extent.height);
        }
        if (extent.width != 0 && extent.height != 0)
        {
            ensureDepthTexture();
        }
        return RhiResult::Ok;
    }

    // ======================================================================
    // MetalDevice
    // ======================================================================

    MetalDevice::MetalDevice(const DeviceDesc& desc)
        : m_log(desc.logCallback, desc.logUserData), m_commands(this)
    {
        m_device = MTLCreateSystemDefaultDevice();
        if (m_device == nil)
        {
            m_log.error("[metal] MTLCreateSystemDefaultDevice 失败：本机没有可用的 Metal 设备");
            return;
        }
        m_queue = [m_device newCommandQueue];
        if (m_queue == nil)
        {
            m_log.error("[metal] MTLCommandQueue 创建失败");
            m_device = nil;
            return;
        }
        queryCapabilities();
    }

    MetalDevice::~MetalDevice()
    {
        if (!m_surfaces.empty())
        {
            // 泄漏是宿主的生命周期错误：surface 必须先于 device 销毁。
            // 这里不代为删除——surface 析构会回调本设备销毁纹理，
            // 在设备析构过程中回调自己是最容易写出 use-after-free 的地方。
            m_log.error("[metal] 设备销毁时仍有 %zu 个表面未释放"
                        "（应在 rxRuntimeDestroy 前先销毁全部 Surface）",
                        m_surfaces.size());
        }
        m_surfaces.clear();
        m_bindGroups.clear();
        m_shaders.clear();
        m_samplers.clear();
        m_textures.clear();
        m_buffers.clear();
        m_queue = nil;
        m_device = nil;
    }

    void MetalDevice::queryCapabilities()
    {
        m_caps.backend = BackendKind::Metal;
        m_caps.acceptedShaderLanguage = ShaderLanguage::MetalLib;

        NSString* name = [m_device name];
        if (name != nil)
        {
            std::snprintf(m_caps.deviceName, sizeof(m_caps.deviceName), "%s", [name UTF8String]);
        }

        // Metal 与 macOS 上的 GL CoreProfile 一样不支持宽线，粗线必须三角化
        m_caps.maxLineWidth = 1.0f;

        // M1 只做设备/表面/资源，管线与计算尚未实现，能力位据实报 false。
        // M2 完成后开 indirectDraw，M3 完成后开 computeShaders —— 能力位与
        // 实际可用的调用路径必须一致，否则上层会走进一条只报错的路。
        m_caps.computeShaders = false;
        m_caps.indirectDraw = false;
        m_caps.multiDrawIndirect = false;

        m_caps.storageBuffers = true;
        // Metal 没有多边形线框模式（VK_POLYGON_MODE_LINE 无对应物），
        // 线框必须由上层三角化后以 LineList 提交
        m_caps.wireframeFill = false;
        m_caps.baseVertexOffset = true;
        m_caps.persistentMapping = true;
        m_caps.timestampQueries = false;

        m_caps.maxTextureSize = 16384;
        m_caps.maxVertexAttributes = kMaxVertexAttributes;
        m_caps.maxColorAttachments = 8;
        // setVertexBytes / setFragmentBytes 的上限是 4KB，远大于 RHI 的 128 字节
        m_caps.maxPushConstantBytes = 4096;
        m_caps.uniformBufferOffsetAlignment = 256;
        m_caps.storageBufferOffsetAlignment = 256;
        m_caps.maxFramesInFlight = 3;

        m_log.info("[metal] 设备就绪：%s", m_caps.deviceName);
    }

    ISurface* MetalDevice::createSurface(const SurfaceDesc& desc)
    {
        if (desc.window.kind != NativeWindow::Kind::CocoaNsView)
        {
            m_log.error("[metal] createSurface: 仅支持 CocoaNsView（handleA = NSView*），"
                        "收到 kind=%d。其余窗口形态不在本后端职责内。",
                        static_cast<int>(desc.window.kind));
            return nullptr;
        }
        if (m_device == nil)
        {
            m_log.error("[metal] createSurface: 设备不可用");
            return nullptr;
        }

        auto* surface = new MetalSurface(this, desc, m_log);
        if (surface->layer() == nil)
        {
            delete surface;
            return nullptr;
        }
        m_surfaces.push_back(surface);
        m_log.debug("[metal] createSurface: %ux%u (surface count: %zu)",  // 创建表面
                    desc.initialExtent.width, desc.initialExtent.height, m_surfaces.size());
        return surface;
    }

    void MetalDevice::destroySurface(ISurface* surface)
    {
        if (!surface)
        {
            return;
        }
        auto* metalSurface = static_cast<MetalSurface*>(surface);
        for (size_t i = 0; i < m_surfaces.size(); ++i)
        {
            if (m_surfaces[i] == metalSurface)
            {
                m_surfaces.erase(m_surfaces.begin() + static_cast<ptrdiff_t>(i));
                delete metalSurface;
                return;
            }
        }
        m_log.warn("[metal] destroySurface: 表面不属于本设备");
    }

    TextureHandle MetalDevice::updateBoundTexture(TextureHandle handle, id<MTLTexture> texture,
                                                  const TextureDesc& desc)
    {
        if (texture == nil)
        {
            return handle;
        }
        MetalTextureRecord* record = m_textures.get(handle);
        if (record == nullptr)
        {
            MetalTextureRecord created{};
            created.texture = texture;
            created.desc = desc;
            return m_textures.add(std::move(created));
        }
        // 句柄复用：只换底层纹理，尺寸/格式随每帧的 drawable 一起更新
        record->texture = texture;
        record->desc = desc;
        return handle;
    }

    ShaderHandle MetalDevice::createShader(const ShaderDesc& desc)
    {
        if (desc.data == nullptr || desc.sizeBytes == 0)
        {
            m_log.error("[metal] createShader: 数据为空");
            return ShaderHandle{};
        }
        if (desc.language != ShaderLanguage::MetalLib && desc.language != ShaderLanguage::MetalSource)
        {
            m_log.error("[metal] createShader: 语言 %d 不是 Metal 可接受的形式（需要 metallib 或 MSL）",
                        static_cast<int>(desc.language));
            return ShaderHandle{};
        }

        MetalShaderRecord record{};
        record.language = desc.language;
        record.bytes.resize(static_cast<size_t>(desc.sizeBytes));
        std::memcpy(record.bytes.data(), desc.data, static_cast<size_t>(desc.sizeBytes));
        return m_shaders.add(std::move(record));
    }

    void MetalDevice::destroyShader(ShaderHandle shader)
    {
        if (!m_shaders.remove(shader))
        {
            m_log.warn("[metal] destroyShader: 句柄无效或已销毁");
        }
    }

    PipelineHandle MetalDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
    {
        // M2 落地：MTLRenderPipelineDescriptor + MTLVertexDescriptor 映射。
        // 现阶段明确失败，避免上层拿到一个「看起来有效但画不出东西」的管线。
        (void)desc;
        m_log.error("[metal] createGraphicsPipeline 尚未实现（M2）。当前版本只支持设备/表面/资源。");
        return PipelineHandle{};
    }

    PipelineHandle MetalDevice::createComputePipeline(const ComputePipelineDesc& desc)
    {
        // M3 落地：newComputePipelineStateWithFunction
        (void)desc;
        m_log.error("[metal] createComputePipeline 尚未实现（M3）。");
        return PipelineHandle{};
    }

    void MetalDevice::destroyPipeline(PipelineHandle pipeline)
    {
        (void)pipeline;
        m_log.warn("[metal] destroyPipeline: 管线尚未实现（M2）");
    }

    BufferHandle MetalDevice::createBuffer(const BufferDesc& desc)
    {
        if (desc.size == 0)
        {
            m_log.error("[metal] createBuffer: size 为 0");
            return BufferHandle{};
        }

        const MTLResourceOptions options = toResourceOptions(desc.access);
        id<MTLBuffer> buffer = [m_device newBufferWithLength:desc.size options:options];
        if (buffer == nil)
        {
            m_log.error("[metal] createBuffer 失败（%llu 字节）",
                        static_cast<unsigned long long>(desc.size));
            return BufferHandle{};
        }
        if (desc.debugName != nullptr)
        {
            buffer.label = [NSString stringWithUTF8String:desc.debugName];
        }

        MetalBufferRecord record{};
        record.buffer = buffer;
        record.desc = desc;
        return m_buffers.add(std::move(record));
    }

    void MetalDevice::destroyBuffer(BufferHandle buffer)
    {
        if (!m_buffers.remove(buffer))
        {
            m_log.warn("[metal] destroyBuffer: 句柄无效或已销毁");
        }
    }

    RhiResult MetalDevice::writeBuffer(BufferHandle buffer, uint64_t offset, const void* data,
                                       uint64_t sizeBytes)
    {
        MetalBufferRecord* record = m_buffers.get(buffer);
        if (record == nullptr || record->buffer == nil)
        {
            m_log.error("[metal] writeBuffer: 缓冲区句柄无效");
            return RhiResult::ErrorInvalidArgument;
        }
        if (data == nullptr || sizeBytes == 0)
        {
            m_log.error("[metal] writeBuffer: 数据为空");
            return RhiResult::ErrorInvalidArgument;
        }
        if (offset + sizeBytes > record->desc.size)
        {
            m_log.error("[metal] writeBuffer: 越界（缓冲 %llu 字节，请求 %llu+%llu）",
                        static_cast<unsigned long long>(record->desc.size),
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(sizeBytes));
            return RhiResult::ErrorInvalidArgument;
        }
        // Shared 存储：直接写 contents。GPU 与 CPU 的一致性由 Metal 在
        // 命令缓冲边界上保证，不需要 GL 那样的显式 flush。
        std::memcpy(static_cast<uint8_t*>([record->buffer contents]) + offset, data,
                    static_cast<size_t>(sizeBytes));
        return RhiResult::Ok;
    }

    MappedRange MetalDevice::mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes)
    {
        MappedRange range{};
        MetalBufferRecord* record = m_buffers.get(buffer);
        if (record == nullptr || record->buffer == nil)
        {
            m_log.error("[metal] mapBuffer: 缓冲区句柄无效");
            return range;
        }
        if (offset + sizeBytes > record->desc.size)
        {
            m_log.error("[metal] mapBuffer: 越界（缓冲 %llu 字节，请求 %llu+%llu）",
                        static_cast<unsigned long long>(record->desc.size),
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(sizeBytes));
            return range;
        }
        range.ptr = static_cast<uint8_t*>([record->buffer contents]) + offset;
        range.offset = offset;
        range.size = sizeBytes;
        return range;
    }

    void MetalDevice::unmapBuffer(BufferHandle buffer)
    {
        // Shared 存储没有「解除映射」的动作：缓冲始终可写。
        // 保留本函数是为了满足 RHI 契约（调用方不应依赖它的副作用）。
        (void)buffer;
    }

    void MetalDevice::flushMappedRange(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes)
    {
        // Shared 存储由 Metal 在命令缓冲边界上自动保证可见性，
        // 不需要 glFlushMappedBufferRange 那样的显式刷写。
        (void)buffer;
        (void)offset;
        (void)sizeBytes;
    }

    TextureHandle MetalDevice::createTexture(const TextureDesc& desc)
    {
        MTLTextureDescriptor* native = [MTLTextureDescriptor new];
        native.textureType = MTLTextureType2D;
        native.pixelFormat = toMetalFormat(desc.format);
        native.width = desc.width;
        native.height = desc.height;
        native.mipmapLevelCount = desc.mipLevels != 0 ? desc.mipLevels : 1;
        native.arrayLength = desc.arrayLayers != 0 ? desc.arrayLayers : 1;

        MTLTextureUsage usage = MTLTextureUsageUnknown;
        if (hasFlag(desc.usage, TextureUsage::Sampled))
        {
            usage |= MTLTextureUsageShaderRead;
        }
        if (hasFlag(desc.usage, TextureUsage::ColorAttachment))
        {
            usage |= MTLTextureUsageRenderTarget;
        }
        if (hasFlag(desc.usage, TextureUsage::DepthStencilAttachment))
        {
            usage |= MTLTextureUsageRenderTarget;
        }
        if (hasFlag(desc.usage, TextureUsage::Storage))
        {
            usage |= MTLTextureUsageShaderWrite;
        }
        if (hasFlag(desc.usage, TextureUsage::TransferSrc) ||
            hasFlag(desc.usage, TextureUsage::TransferDst))
        {
            usage |= MTLTextureUsageShaderRead;
        }
        native.usage = usage;
        native.storageMode = MTLStorageModePrivate;

        id<MTLTexture> texture = [m_device newTextureWithDescriptor:native];
        if (texture == nil)
        {
            m_log.error("[metal] createTexture 失败（%ux%u）", desc.width, desc.height);
            return TextureHandle{};
        }
        if (desc.debugName != nullptr)
        {
            texture.label = [NSString stringWithUTF8String:desc.debugName];
        }

        MetalTextureRecord record{};
        record.texture = texture;
        record.desc = desc;
        return m_textures.add(std::move(record));
    }

    void MetalDevice::destroyTexture(TextureHandle texture)
    {
        if (!m_textures.remove(texture))
        {
            m_log.warn("[metal] destroyTexture: 句柄无效或已销毁");
        }
    }

    RhiResult MetalDevice::writeTexture(TextureHandle texture, uint32_t mipLevel, const Rect2D& region,
                                        const void* data, uint64_t sizeBytes)
    {
        MetalTextureRecord* record = m_textures.get(texture);
        if (record == nullptr || record->texture == nil)
        {
            m_log.error("[metal] writeTexture: 纹理句柄无效");
            return RhiResult::ErrorInvalidArgument;
        }
        if (data == nullptr || region.width == 0 || region.height == 0)
        {
            m_log.error("[metal] writeTexture: 区域或数据为空");
            return RhiResult::ErrorInvalidArgument;
        }

        const uint32_t bytesPerPixel = formatByteSize(record->desc.format);
        const uint32_t tightPitch = region.width * bytesPerPixel;
        const uint64_t required = static_cast<uint64_t>(tightPitch) * region.height;
        if (sizeBytes < required)
        {
            m_log.error("[metal] writeTexture: 数据不足（需要 %llu，给了 %llu）",
                        static_cast<unsigned long long>(required),
                        static_cast<unsigned long long>(sizeBytes));
            return RhiResult::ErrorInvalidArgument;
        }

        // Private 纹理不能直接写：先写进 Shared 暂存缓冲再 blit。
        // replaceRegion 只对 Shared/Managed 纹理有效。
        const uint32_t alignedPitch = alignUp(tightPitch, kReadbackPitchAlignment);
        id<MTLBuffer> staging = [m_device newBufferWithLength:static_cast<NSUInteger>(alignedPitch) *
                                                              region.height
                                                      options:MTLResourceStorageModeShared];
        if (staging == nil)
        {
            m_log.error("[metal] writeTexture: 暂存缓冲分配失败");
            return RhiResult::ErrorOutOfMemory;
        }

        uint8_t* dst = static_cast<uint8_t*>([staging contents]);
        if (alignedPitch == tightPitch)
        {
            std::memcpy(dst, data, static_cast<size_t>(required));
        }
        else
        {
            const uint8_t* src = static_cast<const uint8_t*>(data);
            for (uint32_t row = 0; row < region.height; ++row)
            {
                std::memcpy(dst + static_cast<size_t>(row) * alignedPitch,
                            src + static_cast<size_t>(row) * tightPitch, tightPitch);
            }
        }

        id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:staging
                sourceOffset:0
           sourceBytesPerRow:alignedPitch
         sourceBytesPerImage:static_cast<NSUInteger>(alignedPitch) * region.height
                  sourceSize:MTLSizeMake(region.width, region.height, 1)
                   toTexture:record->texture
            destinationSlice:0
            destinationLevel:mipLevel
           destinationOrigin:MTLOriginMake(region.x, region.y, 0)];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        return RhiResult::Ok;
    }

    SamplerHandle MetalDevice::createSampler(const SamplerDesc& desc)
    {
        MTLSamplerDescriptor* native = [MTLSamplerDescriptor new];
        native.minFilter = toMetalFilter(desc.minFilter);
        native.magFilter = toMetalFilter(desc.magFilter);
        native.mipFilter = desc.mipFilter == FilterMode::Nearest ? MTLSamplerMipFilterNearest
                                                                 : MTLSamplerMipFilterLinear;
        native.sAddressMode = toMetalAddressMode(desc.addressU);
        native.tAddressMode = toMetalAddressMode(desc.addressV);
        native.maxAnisotropy = static_cast<NSUInteger>(desc.maxAnisotropy > 1.0f ? desc.maxAnisotropy
                                                                                : 1.0f);

        id<MTLSamplerState> sampler = [m_device newSamplerStateWithDescriptor:native];
        if (sampler == nil)
        {
            m_log.error("[metal] createSampler 失败");
            return SamplerHandle{};
        }

        MetalSamplerRecord record{};
        record.sampler = sampler;
        record.desc = desc;
        return m_samplers.add(std::move(record));
    }

    void MetalDevice::destroySampler(SamplerHandle sampler)
    {
        if (!m_samplers.remove(sampler))
        {
            m_log.warn("[metal] destroySampler: 句柄无效或已销毁");
        }
    }

    BindGroupHandle MetalDevice::createBindGroup(const BindGroupDesc& desc)
    {
        MetalBindGroupRecord record{};
        if (desc.buffers != nullptr)
        {
            record.buffers.assign(desc.buffers, desc.buffers + desc.bufferCount);
        }
        if (desc.textures != nullptr)
        {
            record.textures.assign(desc.textures, desc.textures + desc.textureCount);
        }
        return m_bindGroups.add(std::move(record));
    }

    void MetalDevice::destroyBindGroup(BindGroupHandle group)
    {
        if (!m_bindGroups.remove(group))
        {
            m_log.warn("[metal] destroyBindGroup: 句柄无效或已销毁");
        }
    }

    ICommandList* MetalDevice::beginFrame(ISurface* surface)
    {
        if (surface == nullptr)
        {
            m_log.error("[metal] beginFrame: surface 为空");
            return nullptr;
        }
        if (m_inFrame)
        {
            m_log.error("[metal] beginFrame: 上一帧未 submitFrame");
            return nullptr;
        }

        auto* metalSurface = static_cast<MetalSurface*>(surface);
        bool owned = false;
        for (MetalSurface* s : m_surfaces)
        {
            owned = owned || (s == metalSurface);
        }
        if (!owned)
        {
            m_log.error("[metal] beginFrame: 表面不属于本设备");
            return nullptr;
        }

        id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
        if (commandBuffer == nil)
        {
            m_log.error("[metal] beginFrame: MTLCommandBuffer 创建失败");
            return nullptr;
        }

        m_frameSurface = metalSurface;
        m_inFrame = true;
        ++m_frameIndex;
        m_commands.beginFrame(metalSurface, commandBuffer);
        return &m_commands;
    }

    RhiResult MetalDevice::submitFrame()
    {
        if (!m_inFrame)
        {
            m_log.error("[metal] submitFrame: 本帧未 beginFrame");
            return RhiResult::ErrorUnknown;
        }
        // Metal 的提交与呈现无法分离（presentDrawable 必须在 commit 之前设置），
        // 真正的 commit 在 MetalSurface::present 里完成。这里只负责收拢帧状态。
        m_inFrame = false;
        m_frameSurface = nullptr;
        return RhiResult::Ok;
    }

    void MetalDevice::waitIdle()
    {
        // Metal 没有设备级 waitIdle。等一个空命令缓冲完成即可保证
        // 此前提交的所有工作都已结束——这是官方推荐的做法。
        id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    }

    RhiResult MetalDevice::readTexture(TextureHandle texture, const Rect2D& region, void* outPixels,
                                       uint64_t bufferSize, uint32_t* outRowPitch)
    {
        MetalTextureRecord* record = m_textures.get(texture);
        if (record == nullptr || record->texture == nil)
        {
            m_log.error("[metal] readTexture: 纹理句柄无效");
            return RhiResult::ErrorInvalidArgument;
        }
        if (outPixels == nullptr || region.width == 0 || region.height == 0)
        {
            m_log.error("[metal] readTexture: 输出缓冲为空或区域为 0");
            return RhiResult::ErrorInvalidArgument;
        }

        const uint32_t bytesPerPixel = formatByteSize(record->desc.format);
        const uint32_t tightPitch = region.width * bytesPerPixel;
        const uint64_t required = static_cast<uint64_t>(tightPitch) * region.height;
        if (bufferSize < required)
        {
            m_log.error("[metal] readTexture: 输出缓冲不足（需要 %llu，给了 %llu）",
                        static_cast<unsigned long long>(required),
                        static_cast<unsigned long long>(bufferSize));
            return RhiResult::ErrorInvalidArgument;
        }

        // blit 的目标行距必须按 256 对齐，因此暂存缓冲比紧凑尺寸大；
        // 拷给调用方时再逐行压紧——调用方的缓冲只保证 width*4 的紧凑容量。
        const uint32_t alignedPitch = alignUp(tightPitch, kReadbackPitchAlignment);
        const NSUInteger stagingSize = static_cast<NSUInteger>(alignedPitch) * region.height;

        id<MTLBuffer> staging = [m_device newBufferWithLength:stagingSize
                                                      options:MTLResourceStorageModeShared];
        if (staging == nil)
        {
            m_log.error("[metal] readTexture: 暂存缓冲分配失败");
            return RhiResult::ErrorOutOfMemory;
        }

        id<MTLCommandBuffer> commandBuffer = [m_queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromTexture:record->texture
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(region.x, region.y, 0)
                   sourceSize:MTLSizeMake(region.width, region.height, 1)
                     toBuffer:staging
            destinationOffset:0
       destinationBytesPerRow:alignedPitch
     destinationBytesPerImage:stagingSize];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        const uint8_t* src = static_cast<const uint8_t*>([staging contents]);
        auto* dst = static_cast<uint8_t*>(outPixels);
        // Metal 的纹理原点与 CPU 图像一致（左上角），不做 GL 那样的翻转
        if (alignedPitch == tightPitch)
        {
            std::memcpy(dst, src, static_cast<size_t>(required));
        }
        else
        {
            for (uint32_t row = 0; row < region.height; ++row)
            {
                std::memcpy(dst + static_cast<size_t>(row) * tightPitch,
                            src + static_cast<size_t>(row) * alignedPitch, tightPitch);
            }
        }

        if (outRowPitch != nullptr)
        {
            *outRowPitch = tightPitch;
        }
        return RhiResult::Ok;
    }

    uint64_t MetalDevice::gpuMemoryUsageBytes() const
    {
        // 与 GlDevice 同一做法：资源池只有非 const 迭代器，用 const_cast 取用。
        // 这里只读，不修改任何记录。
        auto* self = const_cast<MetalDevice*>(this);
        uint64_t total = 0;
        for (const MetalBufferRecord* record = self->m_buffers.begin(); record != self->m_buffers.end();
             ++record)
        {
            total += record->desc.size;
        }
        for (const MetalTextureRecord* record = self->m_textures.begin();
             record != self->m_textures.end(); ++record)
        {
            const uint32_t levels = record->desc.mipLevels != 0 ? record->desc.mipLevels : 1;
            total += static_cast<uint64_t>(record->desc.width) * record->desc.height *
                     formatByteSize(record->desc.format) * levels;
        }
        return total;
    }

}  // namespace Render::RHI::metal

namespace Render::RHI
{

    IGpuDevice* createMetalDevice(const DeviceDesc& desc)
    {
        RhiLogger logger(desc.logCallback, desc.logUserData);

        auto* device = new metal::MetalDevice(desc);
        if (device->nativeDevice() == nil)
        {
            // 构造函数已经说明了具体原因（无 Metal 设备 / 命令队列创建失败），
            // 这里不再重复报错，避免同一故障刷出两条日志。
            delete device;
            return nullptr;
        }
        logger.debug("[metal] createMetalDevice 成功（%s）", device->capabilities().deviceName);  // 创建 Metal 设备
        return device;
    }

}  // namespace Render::RHI
