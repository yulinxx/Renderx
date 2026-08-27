/**
 * @file rxRuntime.cpp
 * @brief Runtime 实现：设备、共享资源、管线缓存、瞬态环形缓冲、表面管理
 */

#include "rt/rxInternal.h"

#include "shader/shaderLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace Render::RT::detail
{

    // ==================== 日志桥 ====================

    void LogBridge::forward(RHI::LogLevel level, const char* message, void* userData)
    {
        auto* bridge = static_cast<LogBridge*>(userData);
        if (!bridge || !bridge->callback)
        {
            return;
        }
        LogLevel publicLevel = LogLevel::Info;
        switch (level)
        {
        case RHI::LogLevel::Debug: publicLevel = LogLevel::Debug; break;
        case RHI::LogLevel::Info: publicLevel = LogLevel::Info; break;
        case RHI::LogLevel::Warn: publicLevel = LogLevel::Warn; break;
        case RHI::LogLevel::Error: publicLevel = LogLevel::Error; break;
        }
        bridge->callback(publicLevel, message, bridge->userData);
    }

    // ==================== 枚举与布局翻译 ====================

    RHI::BackendKind toRhiBackend(Backend backend)
    {
        switch (backend)
        {
        case Backend::Null: return RHI::BackendKind::Null;
        case Backend::OpenGL: return RHI::BackendKind::OpenGL;
        case Backend::Metal: return RHI::BackendKind::Metal;
        case Backend::Vulkan: return RHI::BackendKind::Vulkan;
        case Backend::Auto: return RHI::preferredBackend();
        }
        return RHI::BackendKind::OpenGL;
    }

    RHI::PrimitiveTopology toRhiTopology(PrimitiveTopology topology)
    {
        switch (topology)
        {
        case PrimitiveTopology::Points: return RHI::PrimitiveTopology::PointList;
        case PrimitiveTopology::Lines: return RHI::PrimitiveTopology::LineList;
        case PrimitiveTopology::LineStrip: return RHI::PrimitiveTopology::LineStrip;
        // LineLoop 在 Metal/Vulkan 都没有原生对应，RHI 因此不提供它。
        // 退化为 LineStrip：调用方若需要闭合，应自己把首点补到末尾。
        case PrimitiveTopology::LineLoop: return RHI::PrimitiveTopology::LineStrip;
        case PrimitiveTopology::Triangles: return RHI::PrimitiveTopology::TriangleList;
        case PrimitiveTopology::TriangleStrip: return RHI::PrimitiveTopology::TriangleStrip;
        }
        return RHI::PrimitiveTopology::TriangleList;
    }

    RHI::BlendFactor toRhiBlendFactor(BlendFactor factor)
    {
        switch (factor)
        {
        case BlendFactor::Zero: return RHI::BlendFactor::Zero;
        case BlendFactor::One: return RHI::BlendFactor::One;
        case BlendFactor::SrcAlpha: return RHI::BlendFactor::SrcAlpha;
        case BlendFactor::OneMinusSrcAlpha: return RHI::BlendFactor::OneMinusSrcAlpha;
        }
        return RHI::BlendFactor::One;
    }

    RHI::CompareOp toRhiCompareOp(DepthFunc func)
    {
        switch (func)
        {
        case DepthFunc::Always: return RHI::CompareOp::Always;
        case DepthFunc::Less: return RHI::CompareOp::Less;
        case DepthFunc::LessEqual: return RHI::CompareOp::LessEqual;
        case DepthFunc::Greater: return RHI::CompareOp::Greater;
        }
        return RHI::CompareOp::LessEqual;
    }

    VertexLayout vertexLayoutFor(VertexFormat format)
    {
        using AT = RHI::VertexAttribType;
        VertexLayout layout{};
        layout.stride = rxVertexStride(format);

        auto attr = [](uint32_t location, uint32_t offset, AT type) {
            RHI::VertexAttribute a{};
            a.location = location;
            a.bufferSlot = 0;
            a.offset = offset;
            a.type = type;
            return a;
        };

        switch (format)
        {
        case VertexFormat::P3C3:
            layout.attributes[0] = attr(0, 0, AT::Float3);
            layout.attributes[1] = attr(1, 12, AT::Float3);
            layout.attributeCount = 2;
            break;
        case VertexFormat::P3C4:
            layout.attributes[0] = attr(0, 0, AT::Float3);
            layout.attributes[1] = attr(1, 12, AT::Float4);
            layout.attributeCount = 2;
            break;
        case VertexFormat::P3N3:
            layout.attributes[0] = attr(0, 0, AT::Float3);
            layout.attributes[1] = attr(1, 12, AT::Float3);
            layout.attributeCount = 2;
            break;
        case VertexFormat::P2T2C4:
            layout.attributes[0] = attr(0, 0, AT::Float2);
            layout.attributes[1] = attr(1, 8, AT::Float2);
            layout.attributes[2] = attr(2, 16, AT::Float4);
            layout.attributeCount = 3;
            break;
        case VertexFormat::P3O2C4:
            // 世界锚点 + 像素偏移 + 颜色，见 renderx.h 的 RenderSpace::WorldPinned
            layout.attributes[0] = attr(0, 0, AT::Float3);
            layout.attributes[1] = attr(1, 12, AT::Float2);
            layout.attributes[2] = attr(2, 20, AT::Float4);
            layout.attributeCount = 3;
            break;
        case VertexFormat::P3T2C4:
            // 世界坐标 + UV + 颜色调制，用于世界空间贴图
            layout.attributes[0] = attr(0, 0, AT::Float3);
            layout.attributes[1] = attr(1, 12, AT::Float2);
            layout.attributes[2] = attr(2, 20, AT::Float4);
            layout.attributeCount = 3;
            break;
        }
        return layout;
    }

    ShaderPair defaultShadersFor(VertexFormat format, RenderSpace space, PrimitiveTopology topology)
    {
        const bool isScreen = space == RenderSpace::Screen;
        const bool isPinned = space == RenderSpace::WorldPinned;
        const bool isPoint = topology == PrimitiveTopology::Points;

        switch (format)
        {
        case VertexFormat::P3O2C4:
            // WorldPinned 只有这一种顶点格式；片段复用 P3C4 的
            return { "world_pinned_p3o2c4.vert", "world_p3c4.frag" };

        case VertexFormat::P2T2C4:
            if (!isScreen)
            {
                // P2T2C4 的顶点着色器把位置当像素坐标（screen_tex_p2t2c4.vert），
                // 世界空间请用 P3T2C4。此前这里无条件返回屏幕变体、忽略 space，
                // 于是 (P2T2C4, World) 会静默建出一条管线却画在错误位置——
                // 不是编译错误，是画错，排查代价远高于直接不给 shader。
                return {};
            }
            return { "screen_tex_p2t2c4.vert", "screen_tex_p2t2c4.frag" };

        case VertexFormat::P3T2C4:
            if (isScreen || isPinned)
            {
                // 世界空间专用：顶点已含世界坐标，屏幕/定尺寸语义无对应着色器
                return {};
            }
            // 片元与 P2T2C4 共用：只吃 vUV/vColor，与空间无关
            return { "world_tex_p3t2c4.vert", "screen_tex_p2t2c4.frag" };

        case VertexFormat::P3C4:
            if (isPinned)
            {
                // 顶点里没有像素偏移字段，无法做定尺寸，明确不给 shader
                return {};
            }
            if (isPoint)
            {
                // 点尺寸只能走 gl_PointSize（像素），因此点有独立的顶点着色器；
                // 片段着色器把方形 sprite 裁成圆形。
                return { isScreen ? "screen_point_p3c4.vert" : "world_point_p3c4.vert",
                         "point_p3c4.frag" };
            }
            return isScreen ? ShaderPair{ "screen_p3c4.vert", "screen_p3c4.frag" }
                            : ShaderPair{ "world_p3c4.vert", "world_p3c4.frag" };

        case VertexFormat::P3C3:
            if (isPinned)
            {
                return {};
            }
            if (isPoint)
            {
                return { isScreen ? "screen_point_p3c3.vert" : "world_point_p3c3.vert",
                         "point_p3c3.frag" };
            }
            return isScreen ? ShaderPair{ "screen_p3c3.vert", "world_p3c3.frag" }
                            : ShaderPair{ "world_p3c3.vert", "world_p3c3.frag" };

        case VertexFormat::P3N3:
            // 3D 网格管线（mesh_3d.*）仍使用 uModelMatrix/uViewMatrix/uProjMatrix
            // 独立 uniform，尚未并入 PushConstants 块。3D 收口是后续阶段的工作，
            // 这里明确不提供，避免出现「建得出来但画不对」的半实现路径。
            return {};
        }
        return {};
    }

    // ==================== PipelineKey ====================

    bool PipelineKey::operator==(const PipelineKey& other) const
    {
        return vertexFormat == other.vertexFormat && space == other.space && topology == other.topology &&
               depthTest == other.depthTest && depthWrite == other.depthWrite &&
               blendEnable == other.blendEnable && srcBlend == other.srcBlend &&
               dstBlend == other.dstBlend && depthFunc == other.depthFunc &&
               lineWidth == other.lineWidth && shaderName == other.shaderName &&
               fragmentShaderName == other.fragmentShaderName;
    }

    size_t PipelineKeyHash::operator()(const PipelineKey& key) const
    {
        // 各枚举都是 uint8，塞进一个 64 位整数后与 shader 名的散列混合。
        uint64_t bits = 0;
        bits |= static_cast<uint64_t>(key.vertexFormat);
        bits |= static_cast<uint64_t>(key.space) << 8;
        bits |= static_cast<uint64_t>(key.topology) << 16;
        bits |= static_cast<uint64_t>(key.depthTest) << 24;
        bits |= static_cast<uint64_t>(key.depthWrite) << 32;
        bits |= static_cast<uint64_t>(key.blendEnable) << 40;
        bits |= static_cast<uint64_t>(key.srcBlend) << 48;
        bits |= static_cast<uint64_t>(key.dstBlend) << 56;
        // depthFunc 与线宽用异或叠加，避免超出 64 位。线宽已量化，
        // 因此按整数倍数散列即可，不必对浮点位模式做散列。
        bits ^= static_cast<uint64_t>(key.depthFunc) * 0x9E3779B97F4A7C15ull;
        bits ^= static_cast<uint64_t>(key.lineWidth / Runtime::kLineWidthQuantum) * 0xC2B2AE3D27D4EB4Full;
        return std::hash<uint64_t>{}(bits) ^ (std::hash<std::string>{}(key.shaderName) << 1) ^
               (std::hash<std::string>{}(key.fragmentShaderName) << 2);
    }

    // ==================== TransientRing ====================

    bool TransientRing::initialize(Runtime* owner, uint64_t capacityBytes)
    {
        m_owner = owner;
        m_device = owner ? owner->device : nullptr;
        m_log = owner ? owner->log : RHI::RhiLogger{};
        m_capacity = capacityBytes;

        if (!m_device)
        {
            return false;
        }

        RHI::BufferDesc desc{};
        // 两段轮转：总大小是单段容量的两倍
        desc.size = m_capacity * kSegmentCount;
        desc.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Index;
        desc.access = RHI::MemoryAccess::CpuToGpu;
        desc.debugName = "RxTransientRing";

        m_buffer = m_device->createBuffer(desc);
        if (!m_buffer.valid())
        {
            m_log.error("[rt] 瞬态环形缓冲创建失败（%llu 字节）",
                        static_cast<unsigned long long>(desc.size));
            return false;
        }
        m_staging.assign(static_cast<size_t>(desc.size), 0);
        m_segment = 0;
        m_segmentBase = 0;
        m_cursor = 0;
        m_flushed = 0;
        return true;
    }

    void TransientRing::releaseOverflowBuffers()
    {
        for (Overflow& overflow : m_overflow)
        {
            // 先摘公共句柄再销毁 RHI 句柄：句柄表里若残留已销毁的缓冲，
            // Runtime::destroy() 末尾的统一清理会再销毁一次。
            if (m_owner && overflow.publicHandle != BufferHandle::Invalid)
            {
                m_owner->buffers.erase(static_cast<uint64_t>(overflow.publicHandle));
                overflow.publicHandle = BufferHandle::Invalid;
            }
            if (m_device && overflow.buffer.valid())
            {
                m_device->destroyBuffer(overflow.buffer);
            }
        }
        m_overflow.clear();
    }

    void TransientRing::shutdown()
    {
        releaseOverflowBuffers();

        // 环形缓冲本体的公共句柄同样登记在 owner 的 buffers 表里
        // （见 Runtime::initialize 里的 setPublicHandle），必须在这里摘除。
        // 几何仓的 shutdown 早就这么做了，瞬态环漏了这一步，结果同一个缓冲
        // 被销毁两次——日志里的 `destroyBuffer: 句柄已失效（重复销毁？）` 就是它。
        if (m_owner && m_publicHandle != BufferHandle::Invalid)
        {
            m_owner->buffers.erase(static_cast<uint64_t>(m_publicHandle));
            m_publicHandle = BufferHandle::Invalid;
        }

        if (m_device && m_buffer.valid())
        {
            m_device->destroyBuffer(m_buffer);
        }
        m_buffer = {};
        m_staging.clear();
        m_device = nullptr;
        m_owner = nullptr;
    }

    void TransientRing::beginFrame()
    {
        releaseOverflowBuffers();

        m_segment = (m_segment + 1) % kSegmentCount;
        m_segmentBase = m_capacity * m_segment;
        m_cursor = m_segmentBase;
        m_flushed = m_cursor;
    }

    bool TransientRing::allocate(uint64_t sizeBytes, TransientAlloc* out)
    {
        if (!out || sizeBytes == 0 || !m_device)
        {
            return false;
        }
        *out = TransientAlloc{};

        // 16 字节对齐：足够任何 float4 顶点属性，也满足多数后端的
        // 顶点缓冲偏移要求
        const uint64_t aligned = (sizeBytes + 15ull) & ~15ull;
        const uint64_t segmentEnd = m_segmentBase + m_capacity;

        if (m_cursor + aligned <= segmentEnd)
        {
            out->buffer = m_publicHandle;
            out->cpuPtr = m_staging.data() + m_cursor;
            out->offset = static_cast<uint32_t>(m_cursor);
            out->sizeBytes = static_cast<uint32_t>(sizeBytes);
            m_cursor += aligned;
            return true;
        }

        // 超出单帧容量：单独开一个缓冲，本帧末释放。
        // 不做回绕覆盖——覆盖已提交命令引用的数据会让画面随机缺块。
        m_log.warn("[rt] 瞬态缓冲单帧容量不足（已用 %llu / %llu 字节），"
                   "为本次 %llu 字节分配临时缓冲。建议增大 RuntimeDesc::transientBufferBytes",
                   static_cast<unsigned long long>(m_cursor - m_segmentBase),
                   static_cast<unsigned long long>(m_capacity),
                   static_cast<unsigned long long>(sizeBytes));

        Overflow overflow{};
        RHI::BufferDesc desc{};
        desc.size = aligned;
        desc.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Index;
        desc.access = RHI::MemoryAccess::CpuToGpu;
        desc.debugName = "RxTransientOverflow";
        overflow.buffer = m_device->createBuffer(desc);
        if (!overflow.buffer.valid())
        {
            m_log.error("[rt] 瞬态溢出缓冲创建失败");
            return false;
        }
        overflow.staging.assign(static_cast<size_t>(aligned), 0);

        if (!m_owner)
        {
            m_device->destroyBuffer(overflow.buffer);
            return false;
        }
        // 溢出缓冲也要有公共句柄，否则 DrawCommand 无法引用它
        overflow.publicHandle = static_cast<BufferHandle>(m_owner->buffers.insert(overflow.buffer));

        out->buffer = overflow.publicHandle;
        out->cpuPtr = overflow.staging.data();
        out->offset = 0;
        out->sizeBytes = static_cast<uint32_t>(sizeBytes);

        m_overflow.push_back(std::move(overflow));
        return true;
    }

    void TransientRing::flush()
    {
        if (!m_device)
        {
            return;
        }
        if (m_cursor > m_flushed)
        {
            m_device->writeBuffer(m_buffer, m_flushed, m_staging.data() + m_flushed, m_cursor - m_flushed);
            m_flushed = m_cursor;
        }
        for (Overflow& overflow : m_overflow)
        {
            if (!overflow.staging.empty())
            {
                m_device->writeBuffer(overflow.buffer, 0, overflow.staging.data(),
                                      overflow.staging.size());
            }
        }
    }

    // ==================== Runtime：创建与销毁 ====================

    bool Runtime::create(const RuntimeDesc& desc)
    {
        logBridge.callback = desc.logCallback;
        logBridge.userData = desc.logUserData;
        log = RHI::RhiLogger(desc.logCallback ? &LogBridge::forward : nullptr, &logBridge);

        RHI::DeviceDesc deviceDesc{};
        deviceDesc.backend = toRhiBackend(desc.backend);
        deviceDesc.enableValidation = desc.enableValidation != 0;
        deviceDesc.logCallback = desc.logCallback ? &LogBridge::forward : nullptr;
        deviceDesc.logUserData = &logBridge;
        deviceDesc.applicationName = desc.applicationName ? desc.applicationName : "RenderX";
        // GL 后端专用：宿主（Qt）注入的符号解析入口。nullptr 时后端用平台默认实现。
        deviceDesc.glGetProcAddress = desc.glGetProcAddress;

        device = RHI::createDevice(deviceDesc);
        if (!device)
        {
            // createDevice 已经通过同一个回调报告了具体原因
            return false;
        }

        // 公共 Capabilities 是内部 Capabilities 的子集投影：
        // 公共 ABI 只暴露调用方真正需要用来决策的项。
        const RHI::Capabilities& rhiCaps = device->capabilities();
        caps = Capabilities{};
        switch (rhiCaps.backend)
        {
        case RHI::BackendKind::Null: caps.backend = Backend::Null; break;
        case RHI::BackendKind::OpenGL: caps.backend = Backend::OpenGL; break;
        case RHI::BackendKind::Metal: caps.backend = Backend::Metal; break;
        case RHI::BackendKind::Vulkan: caps.backend = Backend::Vulkan; break;
        }
        std::snprintf(caps.deviceName, sizeof(caps.deviceName), "%s", rhiCaps.deviceName);
        std::snprintf(caps.driverInfo, sizeof(caps.driverInfo), "%s", rhiCaps.driverInfo);
        caps.computeShaders = rhiCaps.computeShaders ? 1 : 0;
        caps.indirectDraw = rhiCaps.indirectDraw ? 1 : 0;
        caps.storageBuffers = rhiCaps.storageBuffers ? 1 : 0;
        caps.wireframeFill = rhiCaps.wireframeFill ? 1 : 0;
        caps.persistentMapping = rhiCaps.persistentMapping ? 1 : 0;
        caps.timestampQueries = rhiCaps.timestampQueries ? 1 : 0;
        caps.maxLineWidth = rhiCaps.maxLineWidth;
        caps.maxTextureSize = rhiCaps.maxTextureSize;
        caps.maxColorAttachments = rhiCaps.maxColorAttachments;
        caps.uniformBufferOffsetAlignment = rhiCaps.uniformBufferOffsetAlignment;
        caps.maxFramesInFlight = rhiCaps.maxFramesInFlight;

        // 采样器：文本图集与位图都用线性过滤 + 边缘钳制
        RHI::SamplerDesc samplerDesc{};
        samplerDesc.debugName = "RxDefaultSampler";
        defaultSampler = device->createSampler(samplerDesc);

        constexpr uint64_t kDefaultTransientBytes = 64ull * 1024ull * 1024ull;
        // TransientAlloc::offset 是 uint32，而环形缓冲总大小是单段容量的两倍，
        // 因此单段上限为 2GB。超出会让偏移静默回绕，画面表现为随机错位。
        constexpr uint64_t kMaxTransientBytes = 0x80000000ull;
        uint64_t transientBytes =
            desc.transientBufferBytes != 0 ? desc.transientBufferBytes : kDefaultTransientBytes;
        if (transientBytes > kMaxTransientBytes)
        {
            log.warn("[rt] transientBufferBytes=%llu 超过单段上限 %llu，已钳制",
                     static_cast<unsigned long long>(transientBytes),
                     static_cast<unsigned long long>(kMaxTransientBytes));
            transientBytes = kMaxTransientBytes;
        }
        if (!transient.initialize(this, transientBytes))
        {
            destroy();
            return false;
        }        // 环形缓冲本体也要有公共句柄，DrawCommand 才能引用
        transient.setPublicHandle(static_cast<BufferHandle>(buffers.insert(transient.rhiBuffer())));

        // materials[0] 保留：DrawCommand::materialIndex == 0 表示「无材质」
        materials.resize(1);
        // pipelines[0] 保留：pipelineIndex == 0 表示「让 Runtime 自己解析」
        pipelines.resize(1);

        if (!ensureDefaultPipelines())
        {
            log.warn("[rt] 部分内建管线创建失败，相关绘制会被跳过");
        }

        log.info("[rt] Runtime 就绪：backend=%s device=%s 瞬态容量=%llu 字节",
                 rxBackendName(caps.backend), caps.deviceName,
                 static_cast<unsigned long long>(transientBytes));
        return true;
    }

    void Runtime::destroy()
    {
        // 顺序很重要：Session 引用 Surface，Surface 引用设备。
        for (Session* session : sessions)
        {
            if (session)
            {
                session->surface = nullptr;  // 避免 destroy 时回访已销毁的表面
                delete session;
            }
        }
        if (!sessions.empty())
        {
            log.error("[rt] Runtime 销毁时仍有 %zu 个 Session 未销毁（宿主生命周期错误）",
                      sessions.size());
        }
        sessions.clear();

        if (!surfaces.empty())
        {
            log.error("[rt] Runtime 销毁时仍有 %zu 个 Surface 未销毁（宿主生命周期错误）",
                      surfaces.size());
        }
        for (Surface* surface : surfaces)
        {
            if (surface && device && surface->rhi)
            {
                device->destroySurface(surface->rhi);
            }
            delete surface;
        }
        surfaces.clear();

        // 绘制列表先拆：它引用几何仓里的块，反过来则不引用。
        for (DrawList* list : drawLists)
        {
            if (list)
            {
                list->shutdown();
                delete list;
            }
        }
        drawLists.clear();

        // 几何仓的 shutdown 会 device->destroyBuffer 并从 buffers 表摘除自己，
        // 因此必须早于下面的 buffers 统一清理，否则同一个缓冲被销毁两次。
        for (GeometryStore* store : geometryStores)
        {
            if (store)
            {
                store->shutdown();
                delete store;
            }
        }
        geometryStores.clear();

        // 字体的 CPU 侧对象。图集纹理在下面的 textures 循环里统一销毁。
        destroyAllFonts();

        transient.shutdown();

        if (device)
        {
            for (auto& entry : textureBindGroups)
            {
                device->destroyBindGroup(entry.second);
            }
            textureBindGroups.clear();

            // 注意：管线句柄只在 pipelines 里各出现一次。
            // 旧实现同时持有 m_pipelines 与 PipelineStateManager 的缓存，
            // 销毁时两处都释放同一批句柄，构成双重释放。
            for (RHI::PipelineHandle pipeline : pipelines)
            {
                if (pipeline.valid())
                {
                    device->destroyPipeline(pipeline);
                }
            }
            pipelines.clear();
            pipelineCache.clear();

            for (auto& entry : shaders)
            {
                device->destroyShader(entry.second);
            }
            shaders.clear();

            for (RHI::TextureHandle texture : textures)
            {
                device->destroyTexture(texture);
            }
            textures.clear();

            for (RHI::BufferHandle buffer : buffers)
            {
                device->destroyBuffer(buffer);
            }
            buffers.clear();

            if (defaultSampler.valid())
            {
                device->destroySampler(defaultSampler);
                defaultSampler = {};
            }

            RHI::destroyDevice(device);
            device = nullptr;
        }

        materials.clear();
        log.info("[rt] Runtime 已销毁");
    }

    // ==================== Runtime：缓冲 ====================

    BufferHandle Runtime::createBuffer(const BufferDesc& desc)
    {
        if (!device || desc.sizeBytes == 0)
        {
            log.error("[rt] rxBufferCreate: sizeBytes 为 0");
            return BufferHandle::Invalid;
        }
        RHI::BufferDesc rhiDesc{};
        rhiDesc.size = desc.sizeBytes;
        // 公共 ABI 只区分「CPU 是否频繁写」，用途在 RT 侧统一按
        // 顶点+索引开放：2D 路径的同一段瞬态内存会同时用作两者。
        rhiDesc.usage = RHI::BufferUsage::Vertex | RHI::BufferUsage::Index;
        rhiDesc.access = desc.cpuWritable ? RHI::MemoryAccess::CpuToGpu : RHI::MemoryAccess::GpuOnly;
        rhiDesc.debugName = "RxBuffer";

        const RHI::BufferHandle rhi = device->createBuffer(rhiDesc);
        if (!rhi.valid())
        {
            return BufferHandle::Invalid;
        }
        return static_cast<BufferHandle>(buffers.insert(rhi));
    }

    RHI::BufferHandle Runtime::resolveBuffer(BufferHandle handle)
    {
        if (handle == BufferHandle::Invalid)
        {
            return {};
        }
        const RHI::BufferHandle* found = buffers.find(static_cast<uint64_t>(handle));
        return found ? *found : RHI::BufferHandle{};
    }

    void Runtime::destroyBuffer(BufferHandle handle)
    {
        const RHI::BufferHandle rhi = resolveBuffer(handle);
        if (!rhi.valid())
        {
            log.warn("[rt] rxBufferDestroy: 句柄无效或已销毁");
            return;
        }
        if (handle == transient.publicHandle())
        {
            log.error("[rt] rxBufferDestroy: 不能销毁瞬态环形缓冲，它由 Runtime 拥有");
            return;
        }
        device->destroyBuffer(rhi);
        buffers.erase(static_cast<uint64_t>(handle));
    }

    RxResult Runtime::uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t sizeBytes,
                                   const void* data)
    {
        const RHI::BufferHandle rhi = resolveBuffer(handle);
        if (!rhi.valid() || !data || sizeBytes == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        const RHI::RhiResult result = device->writeBuffer(rhi, offset, data, sizeBytes);
        return result == RHI::RhiResult::Ok ? RxResult::Ok : RxResult::ErrorInvalidArgument;
    }

    // ==================== Runtime：纹理与材质 ====================

    TextureHandle Runtime::createTexture(const TextureDesc& desc)
    {
        if (!device || desc.width == 0 || desc.height == 0)
        {
            log.error("[rt] rxTextureCreate: 尺寸为 0");
            return TextureHandle::Invalid;
        }
        RHI::TextureDesc rhiDesc{};
        rhiDesc.width = desc.width;
        rhiDesc.height = desc.height;
        rhiDesc.format = RHI::Format::RGBA8Unorm;
        rhiDesc.usage = RHI::TextureUsage::Sampled;
        rhiDesc.debugName = "RxTexture";

        const RHI::TextureHandle rhi = device->createTexture(rhiDesc);
        if (!rhi.valid())
        {
            return TextureHandle::Invalid;
        }
        const auto handle = static_cast<TextureHandle>(textures.insert(rhi));

        if (desc.rgba && desc.rgbaBytes > 0)
        {
            RHI::Rect2D region{ 0, 0, desc.width, desc.height };
            device->writeTexture(rhi, 0, region, desc.rgba, desc.rgbaBytes);
        }
        return handle;
    }

    void Runtime::destroyTexture(TextureHandle handle)
    {
        const RHI::TextureHandle* found = textures.find(static_cast<uint64_t>(handle));
        if (!found)
        {
            log.warn("[rt] rxTextureDestroy: 句柄无效或已销毁");
            return;
        }
        // 该纹理的绑定组随之失效，必须一并销毁，否则下次同句柄值
        // 复用时会拿到指向旧纹理的绑定组
        auto cached = textureBindGroups.find(static_cast<uint64_t>(handle));
        if (cached != textureBindGroups.end())
        {
            device->destroyBindGroup(cached->second);
            textureBindGroups.erase(cached);
        }
        device->destroyTexture(*found);
        textures.erase(static_cast<uint64_t>(handle));
    }

    RxResult Runtime::updateTexture(TextureHandle handle, const TextureDesc& desc)
    {
        const RHI::TextureHandle* found = textures.find(static_cast<uint64_t>(handle));
        if (!found || !desc.rgba || desc.rgbaBytes == 0)
        {
            return RxResult::ErrorInvalidArgument;
        }
        RHI::Rect2D region{ 0, 0, desc.width, desc.height };
        const RHI::RhiResult result = device->writeTexture(*found, 0, region, desc.rgba, desc.rgbaBytes);
        return result == RHI::RhiResult::Ok ? RxResult::Ok : RxResult::ErrorInvalidArgument;
    }

    RHI::BindGroupHandle Runtime::bindGroupForTexture(TextureHandle handle)
    {
        const auto key = static_cast<uint64_t>(handle);
        auto cached = textureBindGroups.find(key);
        if (cached != textureBindGroups.end())
        {
            return cached->second;
        }
        const RHI::TextureHandle* found = textures.find(key);
        if (!found)
        {
            return {};
        }

        RHI::TextureBinding binding{};
        binding.set = 0;
        binding.binding = 0;
        binding.texture = *found;
        binding.sampler = defaultSampler;

        RHI::BindGroupDesc desc{};
        desc.textures = &binding;
        desc.textureCount = 1;
        desc.debugName = "RxTextureBindGroup";

        const RHI::BindGroupHandle group = device->createBindGroup(desc);
        if (group.valid())
        {
            textureBindGroups.emplace(key, group);
        }
        return group;
    }

    uint16_t Runtime::addMaterial(const MaterialDesc& desc)
    {
        if (materials.size() >= 0xFFFF)
        {
            log.error("[rt] rxMaterialAdd: 材质数量已达 uint16 上限");
            return 0;
        }
        materials.push_back(desc);
        return static_cast<uint16_t>(materials.size() - 1);
    }

    RxResult Runtime::updateMaterial(uint16_t index, const MaterialDesc& desc)
    {
        if (index == 0 || index >= materials.size())
        {
            return RxResult::ErrorInvalidArgument;
        }
        materials[index] = desc;
        return RxResult::Ok;
    }

    // ==================== Runtime：增量渲染 ====================

    GeometryStoreHandle Runtime::createGeometryStore(const GeometryStoreDesc& desc)
    {
        if (!device)
        {
            return GeometryStoreHandle::Invalid;
        }
        auto* store = new GeometryStore();
        if (!store->initialize(this, desc))
        {
            // initialize 内部已记录具体原因
            delete store;
            return GeometryStoreHandle::Invalid;
        }
        return static_cast<GeometryStoreHandle>(geometryStores.insert(store));
    }

    GeometryStore* Runtime::resolveGeometryStore(GeometryStoreHandle handle)
    {
        if (handle == GeometryStoreHandle::Invalid)
        {
            return nullptr;
        }
        GeometryStore** found = geometryStores.find(static_cast<uint64_t>(handle));
        return found ? *found : nullptr;
    }

    void Runtime::destroyGeometryStore(GeometryStoreHandle handle)
    {
        GeometryStore* store = resolveGeometryStore(handle);
        if (!store)
        {
            log.warn("[rt] rxGeometryStoreDestroy: 句柄无效或已销毁");
            return;
        }
        // 仓销毁后，任何仍引用它的 DrawCommand 都会在 resolveBuffer 时拿到
        // 无效句柄并被跳过（带 warn），不会崩——但调用方应当先清理 DrawList。
        store->shutdown();
        delete store;
        geometryStores.erase(static_cast<uint64_t>(handle));
    }

    uint64_t Runtime::flushGeometryStores()
    {
        uint64_t uploaded = 0;
        for (GeometryStore* store : geometryStores)
        {
            if (!store)
            {
                continue;
            }
            // 先清零再 flush，于是 uploadBytesThisFrame 恰好是本次的增量。
            // 跨帧累加会让统计随运行时长单调增长，失去意义。
            store->resetFrameCounters();
            store->flush();
            uploaded += store->uploadBytesThisFrame();
        }
        return uploaded;
    }

    DrawListHandle Runtime::createDrawList(const DrawListDesc& desc)
    {
        auto* list = new DrawList();
        if (!list->initialize(this, desc))
        {
            delete list;
            return DrawListHandle::Invalid;
        }
        return static_cast<DrawListHandle>(drawLists.insert(list));
    }

    DrawList* Runtime::resolveDrawList(DrawListHandle handle)
    {
        if (handle == DrawListHandle::Invalid)
        {
            return nullptr;
        }
        DrawList** found = drawLists.find(static_cast<uint64_t>(handle));
        return found ? *found : nullptr;
    }

    void Runtime::destroyDrawList(DrawListHandle handle)
    {
        DrawList* list = resolveDrawList(handle);
        if (!list)
        {
            log.warn("[rt] rxDrawListDestroy: 句柄无效或已销毁");
            return;
        }
        list->shutdown();
        delete list;
        drawLists.erase(static_cast<uint64_t>(handle));
    }

    // ==================== Runtime：管线 ====================

    RHI::ShaderHandle Runtime::shaderByName(const char* name)
    {
        if (!name || !device)
        {
            return {};
        }
        auto cached = shaders.find(name);
        if (cached != shaders.end())
        {
            return cached->second;
        }

        const char* source = shader::glslSource(name);
        if (!source)
        {
            // shaderLibrary 本身不打印日志（保持零业务耦合），
            // 因此这里把可用条目数一并报出来，便于区分「名字写错」
            // 与「shader 根本没被嵌进来」。
            log.error("[rt] 内建 shader \"%s\" 不存在（已嵌入 %u 个条目）", name, shader::count());
            return {};
        }

        RHI::ShaderDesc desc{};
        desc.language = RHI::ShaderLanguage::GlslSource;
        desc.data = source;
        desc.sizeBytes = std::strlen(source);
        desc.debugName = name;

        const RHI::ShaderHandle handle = device->createShader(desc);
        if (handle.valid())
        {
            shaders.emplace(name, handle);
        }
        return handle;
    }

    uint16_t Runtime::createPipelineFromKey(const PipelineKey& key)
    {
        auto cached = pipelineCache.find(key);
        if (cached != pipelineCache.end())
        {
            return cached->second;
        }

        ShaderPair pair = defaultShadersFor(key.vertexFormat, key.space, key.topology);
        if (!key.shaderName.empty())
        {
            pair.vertex = key.shaderName.c_str();
        }
        if (!key.fragmentShaderName.empty())
        {
            pair.fragment = key.fragmentShaderName.c_str();
        }
        if (!pair.vertex || !pair.fragment)
        {
            log.error("[rt] 无法为 vertexFormat=%d space=%d 找到内建 shader"
                      "（P3N3/3D 管线尚未并入 PushConstants 模型）",
                      static_cast<int>(key.vertexFormat), static_cast<int>(key.space));
            return 0;
        }

        const RHI::ShaderHandle vs = shaderByName(pair.vertex);
        const RHI::ShaderHandle fs = shaderByName(pair.fragment);
        if (!vs.valid() || !fs.valid())
        {
            return 0;
        }

        const VertexLayout layout = vertexLayoutFor(key.vertexFormat);
        RHI::VertexBufferLayout bufferLayout{};
        bufferLayout.slot = 0;
        bufferLayout.stride = layout.stride;
        bufferLayout.perInstance = false;

        // 采样器绑定只对带纹理的格式声明
        RHI::BindingSlot textureSlot{};
        textureSlot.set = 0;
        textureSlot.binding = 0;
        textureSlot.type = RHI::BindingType::SampledTexture;
        textureSlot.glName = "uTex";
        const bool needsTexture =
            key.vertexFormat == VertexFormat::P2T2C4 || key.vertexFormat == VertexFormat::P3T2C4;

        RHI::GraphicsPipelineDesc desc{};
        desc.vertexShader = vs;
        desc.fragmentShader = fs;
        desc.topology = toRhiTopology(key.topology);
        desc.attributes = layout.attributes;
        desc.attributeCount = layout.attributeCount;
        desc.bufferLayouts = &bufferLayout;
        desc.bufferLayoutCount = 1;
        desc.bindings = needsTexture ? &textureSlot : nullptr;
        desc.bindingCount = needsTexture ? 1u : 0u;
        desc.pushConstantBytes = kPushConstantBytes;
        desc.raster.lineWidth = key.lineWidth;
        desc.depthStencil.depthTestEnable = key.depthTest != 0;
        desc.depthStencil.depthWriteEnable = key.depthWrite != 0;
        desc.depthStencil.depthCompare = toRhiCompareOp(key.depthFunc);
        desc.blend[0].enable = key.blendEnable != 0;
        desc.blend[0].srcColor = toRhiBlendFactor(key.srcBlend);
        desc.blend[0].dstColor = toRhiBlendFactor(key.dstBlend);
        desc.blend[0].srcAlpha = toRhiBlendFactor(key.srcBlend);
        desc.blend[0].dstAlpha = toRhiBlendFactor(key.dstBlend);
        desc.colorAttachmentCount = 1;
        desc.debugName = pair.vertex;

        const RHI::PipelineHandle handle = device->createGraphicsPipeline(desc);
        if (!handle.valid())
        {
            return 0;
        }
        if (pipelines.size() >= 0xFFFF)
        {
            log.error("[rt] 管线数量已达 uint16 上限");
            device->destroyPipeline(handle);
            return 0;
        }

        pipelines.push_back(handle);
        const auto index = static_cast<uint16_t>(pipelines.size() - 1);
        pipelineCache.emplace(key, index);
        return index;
    }

    uint16_t Runtime::createPipeline(const PipelineDesc& desc)
    {
        PipelineKey key{};
        key.vertexFormat = desc.vertexFormat;
        // 公共 PipelineDesc 没有 space 字段：空间由 shaderName 或
        // 绘制命令的 space 决定。这里按 World 建，屏幕空间管线请走
        // rxPipelineGetDefault 或让 Runtime 按 DrawCommand::space 解析。
        key.space = RenderSpace::World;
        key.topology = desc.topology;
        key.depthTest = desc.depthTest;
        key.depthWrite = desc.depthWrite;
        key.blendEnable = desc.blendEnable;
        key.srcBlend = desc.srcBlend;
        key.dstBlend = desc.dstBlend;
        key.depthFunc = desc.depthFunc;
        key.lineWidth = 1.0f;
        key.shaderName = desc.shaderName ? desc.shaderName : "";
        return createPipelineFromKey(key);
    }

    uint16_t Runtime::resolvePipeline(VertexFormat format, RenderSpace space,
                                      PrimitiveTopology topology, float lineWidth,
                                      const char* fragmentShaderOverride)
    {
        PipelineKey key{};
        key.vertexFormat = format;
        key.space = space;
        key.topology = topology;
        key.depthTest = 0;
        key.depthWrite = 0;
        key.blendEnable = 1;
        key.srcBlend = BlendFactor::SrcAlpha;
        key.dstBlend = BlendFactor::OneMinusSrcAlpha;
        key.depthFunc = DepthFunc::LessEqual;
        if (fragmentShaderOverride)
        {
            key.fragmentShaderName = fragmentShaderOverride;
        }

        // 先按后端上限钳制，再量化。macOS 的 GL 与 Metal 上 maxLineWidth 是 1.0，
        // 于是所有线宽塌缩成一条管线——这正是期望行为：真正需要粗线的调用方
        // 必须自己三角化（Capabilities::maxLineWidth 就是为此暴露的）。
        const float maxWidth = caps.maxLineWidth > 0.0f ? caps.maxLineWidth : 1.0f;
        float clamped = lineWidth > 0.0f ? lineWidth : 1.0f;
        clamped = std::min(clamped, maxWidth);
        const float quantized = std::max(
            kLineWidthQuantum, std::round(clamped / kLineWidthQuantum) * kLineWidthQuantum);
        key.lineWidth = std::min(quantized, maxWidth);

        return createPipelineFromKey(key);
    }

    RHI::PipelineHandle Runtime::rhiPipeline(uint16_t index)
    {
        if (index == 0 || index >= pipelines.size())
        {
            return {};
        }
        return pipelines[index];
    }

    uint16_t Runtime::defaultPipeline(DefaultPipeline kind) const
    {
        const auto index = static_cast<size_t>(kind);
        if (index >= static_cast<size_t>(DefaultPipeline::Count))
        {
            return 0;
        }
        return defaults[index];
    }

    bool Runtime::ensureDefaultPipelines()
    {
        using DP = DefaultPipeline;
        using VF = VertexFormat;
        using RS = RenderSpace;
        using PT = PrimitiveTopology;

        struct Entry
        {
            DP kind;
            VF format;
            RS space;
            PT topology;
            const char* label;
            /// 非空则替换按格式选出的默认片段着色器
            const char* fragmentShader;
        };

        // 与 renderx.h 的 DefaultPipeline 枚举一一对应。
        // 覆盖层统一走 P3C4：缩放时与图元几何一致变换，且支持半透明。
        static const Entry kEntries[] = {
            { DP::WorldLine, VF::P3C3, RS::World, PT::LineStrip, "WorldLine", nullptr },
            { DP::WorldTri, VF::P3C3, RS::World, PT::Triangles, "WorldTri", nullptr },
            { DP::WorldPoint, VF::P3C3, RS::World, PT::Points, "WorldPoint", nullptr },
            { DP::ScreenLine, VF::P3C3, RS::Screen, PT::LineStrip, "ScreenLine", nullptr },
            { DP::ScreenTri, VF::P3C3, RS::Screen, PT::Triangles, "ScreenTri", nullptr },
            { DP::ScreenPoint, VF::P3C3, RS::Screen, PT::Points, "ScreenPoint", nullptr },
            { DP::ScreenTextured, VF::P2T2C4, RS::Screen, PT::Triangles, "ScreenTextured", nullptr },
            { DP::WorldLine4, VF::P3C4, RS::World, PT::LineStrip, "WorldLine4", nullptr },
            { DP::WorldTri4, VF::P3C4, RS::World, PT::Triangles, "WorldTri4", nullptr },
            { DP::WorldPoint4, VF::P3C4, RS::World, PT::Points, "WorldPoint4", nullptr },
            { DP::ScreenLine4, VF::P3C4, RS::Screen, PT::LineStrip, "ScreenLine4", nullptr },
            { DP::ScreenTri4, VF::P3C4, RS::Screen, PT::Triangles, "ScreenTri4", nullptr },
            { DP::ScreenPoint4, VF::P3C4, RS::Screen, PT::Points, "ScreenPoint4", nullptr },
            // 世界锚定 + 屏幕定尺寸（见 renderx.h RenderSpace::WorldPinned）
            { DP::WorldPinnedLine, VF::P3O2C4, RS::WorldPinned, PT::LineStrip, "WorldPinnedLine",
              nullptr },
            { DP::WorldPinnedTri, VF::P3O2C4, RS::WorldPinned, PT::Triangles, "WorldPinnedTri",
              nullptr },
            // 字形：与 ScreenTextured 同格式同空间同拓扑，只有片元不同，
            // 因此必须显式指定片段着色器，否则两者会命中同一条缓存管线。
            { DP::ScreenGlyph, VF::P2T2C4, RS::Screen, PT::Triangles, "ScreenGlyph",
              "screen_glyph_p2t2c4.frag" },
            // 世界空间贴图（位图实体）：顶点是世界坐标，随视图平移/缩放变换。
            // 片元与 ScreenTextured 相同，但空间不同，因此是独立的一条。
            { DP::WorldTextured, VF::P3T2C4, RS::World, PT::Triangles, "WorldTextured", nullptr },
            // 世界空间字形（文字实体）：与 WorldTextured 同格式同空间同拓扑，
            // 只有片元不同（距离场而非 RGBA），因此同样必须显式指定片段着色器。
            { DP::WorldGlyphSdf, VF::P3T2C4, RS::World, PT::Triangles, "WorldGlyphSdf",
              "world_glyph_sdf_p3t2c4.frag" },
        };
        static_assert(sizeof(kEntries) / sizeof(kEntries[0]) == static_cast<size_t>(DP::Count),
                      "内建管线表必须覆盖 DefaultPipeline 的全部取值");

        uint32_t ready = 0;
        for (const Entry& entry : kEntries)
        {
            const uint16_t index =
                resolvePipeline(entry.format, entry.space, entry.topology, 1.0f, entry.fragmentShader);
            defaults[static_cast<size_t>(entry.kind)] = index;
            if (index != 0)
            {
                ready += 1;
            }
            else
            {
                log.error("[rt] 内建管线 %s 创建失败", entry.label);
            }
        }

        log.info("[rt] 内建管线就绪 %u/%u（内嵌 shader %u 个）", ready,
                 static_cast<uint32_t>(DP::Count), shader::count());
        return ready == static_cast<uint32_t>(DP::Count);
    }

    // ==================== Runtime：表面 ====================

    SurfaceHandle Runtime::createSurface(const SurfaceDesc& desc)
    {
        if (!device)
        {
            return SurfaceHandle::Invalid;
        }
        if (desc.width == 0 || desc.height == 0)
        {
            log.error("[rt] rxSurfaceCreate: 尺寸为 0");
            return SurfaceHandle::Invalid;
        }

        RHI::SurfaceDesc rhiDesc{};
        switch (desc.windowKind)
        {
        case NativeWindowKind::None:
            // 无原生窗口：只有 Null 后端能接受（单测/无头）。
            // GL/Metal/Vulkan 的 createSurface 会自行拒绝并报错。
            rhiDesc.window.kind = RHI::NativeWindow::Kind::None;
            break;
        case NativeWindowKind::Win32Hwnd:
            rhiDesc.window.kind = RHI::NativeWindow::Kind::Win32Hwnd;
            break;
        case NativeWindowKind::CocoaNsView:
            rhiDesc.window.kind = RHI::NativeWindow::Kind::CocoaNsView;
            break;
        case NativeWindowKind::XlibWindow:
            rhiDesc.window.kind = RHI::NativeWindow::Kind::XlibWindow;
            break;
        case NativeWindowKind::WaylandSurface:
            rhiDesc.window.kind = RHI::NativeWindow::Kind::WaylandSurface;
            break;
        case NativeWindowKind::ForeignGlContext:
            rhiDesc.window.kind = RHI::NativeWindow::Kind::ForeignGlContext;
            break;
        }
        rhiDesc.window.handleA = desc.handleA;
        rhiDesc.window.handleB = desc.handleB;
        rhiDesc.initialExtent = { desc.width, desc.height };
        rhiDesc.presentMode = desc.presentMode == PresentMode::Immediate
                                  ? RHI::PresentMode::Immediate
                                  : (desc.presentMode == PresentMode::Mailbox ? RHI::PresentMode::Mailbox
                                                                             : RHI::PresentMode::Fifo);
        rhiDesc.depthFormat = desc.enableDepth ? RHI::Format::D32Float : RHI::Format::Unknown;
        rhiDesc.debugName = "RxSurface";

        RHI::ISurface* rhi = device->createSurface(rhiDesc);
        if (!rhi)
        {
            return SurfaceHandle::Invalid;
        }

        auto* surface = new Surface{};
        surface->runtime = this;
        surface->rhi = rhi;
        surface->width = desc.width;
        surface->height = desc.height;
        surface->hasDepth = desc.enableDepth != 0;
        surfaces.push_back(surface);

        return static_cast<SurfaceHandle>(reinterpret_cast<uintptr_t>(surface));
    }

    Surface* Runtime::resolveSurface(SurfaceHandle handle)
    {
        if (handle == SurfaceHandle::Invalid)
        {
            return nullptr;
        }
        auto* candidate = reinterpret_cast<Surface*>(static_cast<uintptr_t>(handle));
        // 必须在自己的列表里找到才认：句柄是裸指针，来自其他 Runtime
        // 或已销毁的表面都必须被拦下，而不是直接解引用。
        for (Surface* surface : surfaces)
        {
            if (surface == candidate)
            {
                return surface;
            }
        }
        log.error("[rt] Surface 句柄不属于本 Runtime（已销毁或来自其他 Runtime）");
        return nullptr;
    }

    void Runtime::destroySurface(SurfaceHandle handle)
    {
        Surface* surface = resolveSurface(handle);
        if (!surface)
        {
            return;
        }
        if (surface->boundSession)
        {
            log.error("[rt] rxSurfaceDestroy: 该表面上仍有 Session，请先销毁 Session");
            return;
        }
        device->destroySurface(surface->rhi);
        surfaces.erase(std::remove(surfaces.begin(), surfaces.end(), surface), surfaces.end());
        delete surface;
    }

    RxResult Runtime::resizeSurface(SurfaceHandle handle, uint32_t width, uint32_t height)
    {
        Surface* surface = resolveSurface(handle);
        if (!surface)
        {
            return RxResult::ErrorInvalidArgument;
        }
        surface->width = width;
        surface->height = height;
        const RHI::RhiResult result = surface->rhi->resize({ width, height });
        return result == RHI::RhiResult::Ok ? RxResult::Ok : RxResult::ErrorInvalidArgument;
    }

}  // namespace Render::RT::detail
