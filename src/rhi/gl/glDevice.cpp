/**
 * @file glDevice.cpp
 * @brief GL 后端：设备与表面实现（命令记录器见 glCommandList.cpp）
 */

#include "rhi/gl/glDevice.h"

#include "rhi/rhiBackendFactory.h"

#include <cstring>

// 少量枚举兜底：Windows 的 <GL/gl.h> 只到 GL 1.1，
// glLoader.h 已兜底大部分，这里补本文件额外用到的。
#ifndef GL_FRAMEBUFFER
    #define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
    #define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#endif
#ifndef GL_DEPTH_STENCIL_ATTACHMENT
    #define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#endif
#ifndef GL_TEXTURE_2D
    #define GL_TEXTURE_2D 0x0DE1
#endif
#ifndef GL_VERTEX_SHADER
    #define GL_VERTEX_SHADER 0x8B31
#endif
#ifndef GL_FRAGMENT_SHADER
    #define GL_FRAGMENT_SHADER 0x8B30
#endif
#ifndef GL_COMPUTE_SHADER
    #define GL_COMPUTE_SHADER 0x91B9
#endif
#ifndef GL_COMPILE_STATUS
    #define GL_COMPILE_STATUS 0x8B81
#endif
#ifndef GL_LINK_STATUS
    #define GL_LINK_STATUS 0x8B82
#endif
#ifndef GL_INFO_LOG_LENGTH
    #define GL_INFO_LOG_LENGTH 0x8B84
#endif
#ifndef GL_RENDERER
    #define GL_RENDERER 0x1F01
#endif
#ifndef GL_VERSION
    #define GL_VERSION 0x1F02
#endif
#ifndef GL_MAX_UNIFORM_BUFFER_BINDINGS
    #define GL_MAX_UNIFORM_BUFFER_BINDINGS 0x8A2F
#endif
#ifndef GL_MAP_READ_BIT
    #define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_MAP_COHERENT_BIT
    #define GL_MAP_COHERENT_BIT 0x0080
#endif
#ifndef GL_MAP_FLUSH_EXPLICIT_BIT
    #define GL_MAP_FLUSH_EXPLICIT_BIT 0x0010
#endif
#ifndef GL_MAP_UNSYNCHRONIZED_BIT
    #define GL_MAP_UNSYNCHRONIZED_BIT 0x0020
#endif
#ifndef GL_DYNAMIC_STORAGE_BIT
    #define GL_DYNAMIC_STORAGE_BIT 0x0100
#endif

namespace Render::RHI::gl
{

    // ==================== GlSurface ====================

    GlSurface::GlSurface(GlDevice* device, const SurfaceDesc& desc, const RhiLogger& logger)
        : m_device(device),
          m_window(desc.window),
          m_extent(desc.initialExtent),
          m_colorFormat(desc.preferredColorFormat),
          m_depthFormat(desc.depthFormat),
          m_presentMode(desc.presentMode),
          m_log(logger)
    {
    }

    RhiResult GlSurface::acquireNextImage()
    {
        if (m_extent.width == 0 || m_extent.height == 0)
        {
            // 窗口最小化：不是错误，但本帧不该渲染。调用方据此跳过。
            return RhiResult::ErrorSwapchainOutOfDate;
        }

        // 关键：宿主（QOpenGLWidget）渲染到自己的 FBO，而不是 0 号帧缓冲。
        // 每帧重新捕获而不是缓存一次——Qt 在 resize / 设备像素比变化时会
        // 重建那个 FBO，缓存下来的旧名字会导致画面停止更新。
        const GLFuncs& f = m_device->gl();
        if (f.GetIntegerv)
        {
            GLint current = 0;
            f.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &current);
            m_defaultFramebuffer = static_cast<GLuint>(current < 0 ? 0 : current);
        }

        m_acquired = true;
        return RhiResult::Ok;
    }

    RhiResult GlSurface::present()
    {
        if (!m_acquired)
        {
            m_log.error("[gl] present: 未先调用 acquireNextImage");
            return RhiResult::ErrorInvalidArgument;
        }
        m_acquired = false;
        m_presentCount += 1;

        // ForeignGlContext：交换缓冲由宿主负责（Qt 在 paintGL 返回后自行 swap）。
        // 这里只保证命令已提交给驱动，避免宿主在未 flush 的情况下 swap。
        const GLFuncs& f = m_device->gl();
        if (f.Flush)
        {
            f.Flush();
        }
        return RhiResult::Ok;
    }

    RhiResult GlSurface::resize(Extent2D extent)
    {
        m_extent = extent;
        // GL 默认帧缓冲的尺寸由窗口系统/宿主管理，这里只记录，
        // 供 beginRenderPass 设置默认视口。
        return RhiResult::Ok;
    }

    // ==================== GlDevice：构造与能力 ====================

    namespace
    {
        /// KHR_debug 回调：把驱动报告的问题原样落进日志。
        ///
        /// 配合 GL_DEBUG_OUTPUT_SYNCHRONOUS 使用时，本函数在**产生问题的那一次
        /// GL 调用内部**被调用，因此日志里紧随其后的就是元凶。没有它的话，
        /// GL 错误要等到下一次 glGetError 才被发现（本后端此前根本没查过），
        /// 而驱动内部崩溃更是一点线索都没有。
        void RENDER_GLAPI glDebugMessageThunk(GLenum source, GLenum type, GLuint id,
            GLenum severity, GLsizei length, const GLchar* message, const void* userParam)
        {
            (void)source;
            (void)length;
            const RhiLogger* log = static_cast<const RhiLogger*>(userParam);
            if (!log || !message)
            {
                return;
            }
            // 通知级刷屏（NVIDIA 会报缓冲区内存位置之类的琐事），降级到 debug
            if (severity == GL_DEBUG_SEVERITY_NOTIFICATION)
            {
                log->debug("[gl][driver] %s", message);
                return;
            }
            // GL_DEBUG_TYPE_OTHER 是驱动的闲聊（"driver allocated storage for
            // renderbuffer 1" 之类），不是问题。它却常带 LOW 严重度，按 warn 打出来会
            // 稀释真正的告警 —— 日志里的 warning 必须条条值得看，否则等于没有告警。
            if (type == GL_DEBUG_TYPE_OTHER)
            {
                log->debug("[gl][driver] %s", message);
                return;
            }
            const char* level = severity == GL_DEBUG_SEVERITY_HIGH     ? "HIGH"
                                : severity == GL_DEBUG_SEVERITY_MEDIUM ? "MEDIUM"
                                                                       : "LOW";
            log->warn("[gl][driver] %s type=0x%04X id=%u: %s", level, type, id, message);
        }
    }  // namespace

    GlDevice::GlDevice(const DeviceDesc& desc, const GLFuncs& functions)
        : m_gl(functions), m_log(desc.logCallback, desc.logUserData), m_commands(this)
    {
        queryCapabilities();

        // pushConstant 用的 UBO：大小固定 kMaxPushConstantBytes，
        // 每帧按需 BufferSubData。128 字节远小于任何对齐要求，无需环形分配。
        if (m_gl.GenBuffers)
        {
            m_gl.GenBuffers(1, &m_pushConstantUbo);
            m_gl.BindBuffer(GL_UNIFORM_BUFFER, m_pushConstantUbo);
            m_gl.BufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(kMaxPushConstantBytes), nullptr,
                            GL_STREAM_DRAW);
            m_gl.BindBuffer(GL_UNIFORM_BUFFER, 0);
        }

        // 点大小必须由顶点着色器决定（gl_PointSize），否则核心 profile 下
        // 只会使用 glPointSize() 的全局值，pushConstant 里的 uPointSize 静默无效。
        // 这是全局开关且只影响点图元，一次性打开即可（枚举的回退定义见 glCommon.h）。
        if (m_gl.Enable)
        {
            m_gl.Enable(GL_PROGRAM_POINT_SIZE);
        }

        m_log.info("[gl] Device created: %s | %s", m_caps.deviceName, m_caps.driverInfo);  // 设备已创建
        // 尽早接上驱动的诊断通道：同步模式下驱动会在出错的那一句调用里回调，
        // 是定位「GL 用法非法」最直接的手段。取不到入口就静默跳过（老驱动/ES）。
        if (m_gl.DebugMessageCallback && m_gl.Enable)
        {
            m_gl.Enable(GL_DEBUG_OUTPUT);
            m_gl.Enable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
            m_gl.DebugMessageCallback(&glDebugMessageThunk, &m_log);
            if (m_gl.DebugMessageControl)
            {
                m_gl.DebugMessageControl(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, nullptr,
                    GL_TRUE);
            }
            m_log.debug("[gl] debug output enabled (synchronous)");
        }
        m_log.debug("[gl] Capabilities: compute=%u indirect=%u multiIndirect=%u storage=%u persistentMap=%u "  // 能力
                    "maxTexture=%u maxUboBindings(1 used for pushConstant)",
                    m_caps.computeShaders, m_caps.indirectDraw, m_caps.multiDrawIndirect,
                    m_caps.storageBuffers, m_caps.persistentMapping, m_caps.maxTextureSize);
    }

    GlDevice::~GlDevice()
    {
        if (!m_surfaces.empty())
        {
            m_log.error("[gl] 设备销毁时仍有 %zu 个表面未销毁（宿主生命周期错误）", m_surfaces.size());
            for (GlSurface* s : m_surfaces)
            {
                delete s;
            }
            m_surfaces.clear();
        }

        deleteAllResources();

        if (m_pushConstantUbo && m_gl.DeleteBuffers)
        {
            m_gl.DeleteBuffers(1, &m_pushConstantUbo);
            m_pushConstantUbo = 0;
        }
        if (m_readbackFbo && m_gl.DeleteFramebuffers)
        {
            m_gl.DeleteFramebuffers(1, &m_readbackFbo);
            m_readbackFbo = 0;
        }
        for (auto& entry : m_framebufferCache)
        {
            if (entry.fbo && m_gl.DeleteFramebuffers)
            {
                m_gl.DeleteFramebuffers(1, &entry.fbo);
            }
        }
        m_framebufferCache.clear();

        m_log.debug("[gl] Device destroyed");  // 设备已销毁
    }

    void GlDevice::queryCapabilities()
    {
        m_caps = Capabilities{};
        m_caps.backend = BackendKind::OpenGL;
        m_caps.acceptedShaderLanguage = ShaderLanguage::GlslSource;

        if (m_gl.GetString)
        {
            const auto* renderer = reinterpret_cast<const char*>(m_gl.GetString(GL_RENDERER));
            const auto* version = reinterpret_cast<const char*>(m_gl.GetString(GL_VERSION));
            std::snprintf(m_caps.deviceName, sizeof(m_caps.deviceName), "%s", renderer ? renderer : "?");
            std::snprintf(m_caps.driverInfo, sizeof(m_caps.driverInfo), "OpenGL %s", version ? version : "?");
        }

        auto queryInt = [this](GLenum name, uint32_t fallback) -> uint32_t {
            if (!m_gl.GetIntegerv)
            {
                return fallback;
            }
            GLint value = 0;
            m_gl.GetIntegerv(name, &value);
            return value > 0 ? static_cast<uint32_t>(value) : fallback;
        };

        m_caps.maxTextureSize = queryInt(GL_MAX_TEXTURE_SIZE, 2048);
        m_caps.maxColorAttachments =
            queryInt(GL_MAX_COLOR_ATTACHMENTS, 1) > kMaxColorAttachments
                ? kMaxColorAttachments
                : queryInt(GL_MAX_COLOR_ATTACHMENTS, 1);
        m_caps.uniformBufferOffsetAlignment = queryInt(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 256);
        m_caps.storageBufferOffsetAlignment = queryInt(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, 256);
        m_caps.maxPushConstantBytes = kMaxPushConstantBytes;

        // 能力按「函数指针是否解析成功」判定，而不是解析 version 字符串：
        // 驱动/上下文配置千差万别，指针存在与否才是能不能调用的唯一依据。
        m_caps.computeShaders = m_gl.DispatchCompute != nullptr && m_gl.MemoryBarrier != nullptr;
        m_caps.indirectDraw = m_gl.DrawArraysIndirect != nullptr && m_gl.DrawElementsIndirect != nullptr;
        m_caps.multiDrawIndirect =
            m_gl.MultiDrawArraysIndirect != nullptr && m_gl.MultiDrawElementsIndirect != nullptr;
        m_caps.storageBuffers = m_gl.BindBufferRange != nullptr && m_caps.computeShaders;
        m_caps.persistentMapping = m_gl.BufferStorage != nullptr && m_gl.MapBufferRange != nullptr;
        m_caps.timestampQueries = false;
        m_caps.wireframeFill = m_gl.PolygonMode != nullptr;  // GLES 没有 glPolygonMode
        m_caps.baseVertexOffset = false;  // glDrawElementsBaseVertex 未纳入函数表
        m_caps.maxFramesInFlight = 1;     // GL 没有显式的帧内飞行概念

        if (m_gl.GetFloatv)
        {
            GLfloat range[2] = { 1.0f, 1.0f };
            m_gl.GetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, range);
            m_caps.maxLineWidth = range[1] >= 1.0f ? range[1] : 1.0f;
        }
        else
        {
            m_caps.maxLineWidth = 1.0f;
        }

        // 前向兼容上下文（forward-compatible）里宽线已被移除：唯一合法值是精确的 1.0，
        // 传 1.0 以外的任何值都会得到 GL_INVALID_VALUE。
        //
        // 坑在于 GL_ALIASED_LINE_WIDTH_RANGE 仍然照旧返回 [1, 10]（NVIDIA 就是这样），
        // 于是"按 caps 夹一下"这种防御完全失效 —— 表现为每帧一条
        // 「GL_INVALID_VALUE ... Operation is not valid from a preview context」
        // （"preview context" 是 NVIDIA 对前向兼容上下文的措辞），而画面看着还正常。
        //
        // 上下文是否前向兼容由宿主决定：Qt 在请求 GL >= 3.0 且未设置
        // QSurfaceFormat::DeprecatedFunctions 时会加上 FORWARD_COMPATIBLE 位。
        // 这里不猜宿主怎么配，直接查 GL_CONTEXT_FLAGS 得到事实，并把 caps 修正成
        // 真实能力 —— 之后所有走 caps 夹取的代码就自动只发 1.0。
        if (m_gl.GetIntegerv)
        {
            GLint contextFlags = 0;
            m_gl.GetIntegerv(GL_CONTEXT_FLAGS, &contextFlags);
            m_forwardCompatible = (contextFlags & GL_CONTEXT_FLAG_FORWARD_COMPATIBLE_BIT) != 0;
            if (m_forwardCompatible && m_caps.maxLineWidth > 1.0f)
            {
                m_log.info("[gl] forward-compatible context: wide lines unavailable, "
                           "clamping maxLineWidth %.1f -> 1.0 (host may set "
                           "QSurfaceFormat::DeprecatedFunctions to keep wide lines)",
                    static_cast<double>(m_caps.maxLineWidth));
                m_caps.maxLineWidth = 1.0f;
            }
        }
    }


    // ==================== 表面 ====================

    ISurface* GlDevice::createSurface(const SurfaceDesc& desc)
    {
        if (desc.window.kind != NativeWindow::Kind::ForeignGlContext)
        {
            m_log.error("[gl] createSurface: 仅支持 ForeignGlContext（宿主自建上下文），"
                        "收到 kind=%d。自建 WGL/GLX/NSOpenGL 上下文不在本后端职责内。",
                        static_cast<int>(desc.window.kind));
            return nullptr;
        }

        auto* surface = new GlSurface(this, desc, m_log);
        m_surfaces.push_back(surface);
        m_log.debug("[gl] createSurface: %ux%u (surface count: %zu)", desc.initialExtent.width,  // 创建表面
                    desc.initialExtent.height, m_surfaces.size());
        return surface;
    }

    void GlDevice::destroySurface(ISurface* surface)
    {
        if (!surface)
        {
            return;
        }
        for (size_t i = 0; i < m_surfaces.size(); ++i)
        {
            if (m_surfaces[i] != surface)
            {
                continue;
            }
            if (m_frameSurface == m_surfaces[i])
            {
                m_log.error("[gl] destroySurface: 该表面正处于 beginFrame 中");
                m_frameSurface = nullptr;
                m_inFrame = false;
            }
            delete m_surfaces[i];
            m_surfaces[i] = m_surfaces.back();
            m_surfaces.pop_back();
            return;
        }
        m_log.error("[gl] destroySurface: 表面不属于本设备");
    }

    // ==================== 着色器与管线 ====================

    ShaderHandle GlDevice::createShader(const ShaderDesc& desc)
    {
        if (!desc.data || desc.sizeBytes == 0)
        {
            m_log.error("[gl] createShader: 空源码");
            return ShaderHandle{};
        }
        if (desc.language != ShaderLanguage::GlslSource)
        {
            m_log.error("[gl] createShader: 本后端只接受 GlslSource，收到 language=%d",
                        static_cast<int>(desc.language));
            return ShaderHandle{};
        }

        GlShaderRecord record{};
        record.language = desc.language;
        const auto* bytes = static_cast<const char*>(desc.data);
        record.source.assign(bytes, bytes + desc.sizeBytes);
        if (record.source.empty() || record.source.back() != '\0')
        {
            record.source.push_back('\0');
        }
        return m_shaders.add(std::move(record));
    }

    void GlDevice::destroyShader(ShaderHandle shader)
    {
        if (shader.valid() && !m_shaders.remove(shader))
        {
            m_log.warn("[gl] destroyShader: 句柄已失效（重复销毁？）");
        }
    }

    GLuint GlDevice::compileStage(GLenum stage, const GlShaderRecord& record, const char* debugName)
    {
        if (!m_gl.CreateShader)
        {
            return 0;
        }
        const GLuint name = m_gl.CreateShader(stage);
        const char* source = record.source.data();
        m_gl.ShaderSource(name, 1, &source, nullptr);
        m_gl.CompileShader(name);

        GLint ok = 0;
        m_gl.GetShaderiv(name, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            // 完整输出 InfoLog：截断日志是旧实现里最常见的调试障碍，
            // GLSL 编译错误经常在后半段。
            GLint length = 0;
            m_gl.GetShaderiv(name, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
            if (length > 0)
            {
                m_gl.GetShaderInfoLog(name, length, nullptr, log.data());
            }
            // 阶段用词而不是只打 GL 枚举：debugName 是「管线名」（取自顶点
            // shader 文件名），三个阶段共用同一个，只看它会把片元阶段的错误
            // 误读成顶点 shader 的错误。
            const char* stageWord = stage == GL_VERTEX_SHADER     ? "vertex"
                                    : stage == GL_FRAGMENT_SHADER ? "fragment"
                                    : stage == GL_COMPUTE_SHADER  ? "compute"
                                                                  : "unknown";
            m_log.error("[gl] 着色器编译失败（stage=%s(0x%04X), pipeline=%s）：%s", stageWord, stage,
                        debugName ? debugName : "?", log.data());
            m_gl.DeleteShader(name);
            return 0;
        }
        return name;
    }

    PipelineHandle GlDevice::createGraphicsPipeline(const GraphicsPipelineDesc& desc)
    {
        const GlShaderRecord* vs = m_shaders.get(desc.vertexShader);
        const GlShaderRecord* fs = m_shaders.get(desc.fragmentShader);
        if (!vs || !fs)
        {
            m_log.error("[gl] createGraphicsPipeline: 顶点或片段着色器句柄无效（vs=%s fs=%s）",
                        vs ? "ok" : "无效", fs ? "ok" : "无效");
            return PipelineHandle{};
        }
        if (desc.attributeCount > kMaxVertexAttributes || desc.bufferLayoutCount > kMaxVertexBufferSlots)
        {
            m_log.error("[gl] createGraphicsPipeline: 顶点属性/绑定槽数量超限（attr=%u slots=%u）",
                        desc.attributeCount, desc.bufferLayoutCount);
            return PipelineHandle{};
        }
        if (desc.pushConstantBytes > kMaxPushConstantBytes)
        {
            m_log.error("[gl] createGraphicsPipeline: pushConstantBytes=%u 超过上限 %u",
                        desc.pushConstantBytes, kMaxPushConstantBytes);
            return PipelineHandle{};
        }

        const GLuint vsName = compileStage(GL_VERTEX_SHADER, *vs, desc.debugName);
        if (!vsName)
        {
            return PipelineHandle{};
        }
        const GLuint fsName = compileStage(GL_FRAGMENT_SHADER, *fs, desc.debugName);
        if (!fsName)
        {
            m_gl.DeleteShader(vsName);
            return PipelineHandle{};
        }

        const GLuint program = m_gl.CreateProgram();
        m_gl.AttachShader(program, vsName);
        m_gl.AttachShader(program, fsName);
        m_gl.LinkProgram(program);
        // 链接完成后 shader 对象即可删除，program 已持有引用
        m_gl.DeleteShader(vsName);
        m_gl.DeleteShader(fsName);

        GLint linked = 0;
        m_gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            GLint length = 0;
            m_gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
            if (length > 0)
            {
                m_gl.GetProgramInfoLog(program, length, nullptr, log.data());
            }
            m_log.error("[gl] 管线链接失败（%s）：%s", desc.debugName ? desc.debugName : "?", log.data());
            m_gl.DeleteProgram(program);
            return PipelineHandle{};
        }

        GlPipelineRecord record{};
        record.program = program;
        record.isCompute = false;
        record.topology = toGlTopology(desc.topology);
        record.attributeCount = desc.attributes ? desc.attributeCount : 0;
        for (uint32_t i = 0; i < record.attributeCount; ++i)
        {
            record.attributes[i] = desc.attributes[i];
        }
        record.bufferLayoutCount = desc.bufferLayouts ? desc.bufferLayoutCount : 0;
        for (uint32_t i = 0; i < record.bufferLayoutCount; ++i)
        {
            record.bufferLayouts[i] = desc.bufferLayouts[i];
        }
        record.raster = desc.raster;
        record.depthStencil = desc.depthStencil;
        // 单目标渲染：只取 0 号附件的混合状态（GL 的独立混合需 4.0+ 的
        // glBlendFuncSeparatei，未纳入函数表）。
        record.blend = desc.blend[0];
        record.pushConstantBytes = desc.pushConstantBytes;

        // ---- (set, binding) → GL 槽位：字符串名字只在这里用一次 ----
        m_gl.UseProgram(program);

        if (desc.pushConstantBytes > 0 && m_gl.GetUniformBlockIndex && m_gl.UniformBlockBinding)
        {
            const GLuint blockIndex = m_gl.GetUniformBlockIndex(program, kPushConstantBlockName);
            if (blockIndex == GL_INVALID_INDEX)
            {
                // 不当作错误：着色器可能仍用独立 uniform（迁移期）。
                // 但要留下明确记录，否则「pushConstants 写了却没生效」很难查。
                m_log.warn("[gl] 管线 %s 声明了 %u 字节 pushConstant，"
                           "但着色器里找不到 uniform block \"%s\"，本管线的 pushConstants 将无效",
                           desc.debugName ? desc.debugName : "?", desc.pushConstantBytes,
                           kPushConstantBlockName);
            }
            else
            {
                m_gl.UniformBlockBinding(program, blockIndex, kPushConstantBinding);
            }
        }

        uint32_t nextUboBinding = kPushConstantBinding + 1;
        uint32_t nextTextureUnit = 0;
        for (uint32_t i = 0; i < desc.bindingCount; ++i)
        {
            const BindingSlot& slot = desc.bindings[i];
            GlBindingMapping mapping{};
            mapping.set = slot.set;
            mapping.binding = slot.binding;
            mapping.type = slot.type;

            switch (slot.type)
            {
            case BindingType::UniformBuffer:
            {
                mapping.glSlot = nextUboBinding++;
                if (slot.glName && m_gl.GetUniformBlockIndex && m_gl.UniformBlockBinding)
                {
                    const GLuint blockIndex = m_gl.GetUniformBlockIndex(program, slot.glName);
                    if (blockIndex == GL_INVALID_INDEX)
                    {
                        m_log.warn("[gl] 管线 %s：uniform block \"%s\"（set=%u binding=%u）未找到",
                                   desc.debugName ? desc.debugName : "?", slot.glName, slot.set,
                                   slot.binding);
                    }
                    else
                    {
                        m_gl.UniformBlockBinding(program, blockIndex, mapping.glSlot);
                    }
                }
                break;
            }
            case BindingType::StorageBuffer:
            {
                mapping.glSlot = nextUboBinding++;
                // SSBO 的块绑定需要 glShaderStorageBlockBinding（GL 4.3），
                // 未纳入函数表；着色器需自行写 layout(binding = N)。
                m_log.debug("[gl] 管线 %s：StorageBuffer set=%u binding=%u 使用 GL binding=%u，"
                            "着色器需显式声明 layout(binding = %u)",
                            desc.debugName ? desc.debugName : "?", slot.set, slot.binding, mapping.glSlot,
                            mapping.glSlot);
                break;
            }
            case BindingType::SampledTexture:
            case BindingType::StorageTexture:
            {
                mapping.glSlot = nextTextureUnit++;
                if (slot.glName && m_gl.GetUniformLocation && m_gl.Uniform1i)
                {
                    const GLint location = m_gl.GetUniformLocation(program, slot.glName);
                    if (location < 0)
                    {
                        m_log.warn("[gl] 管线 %s：采样器 uniform \"%s\"（set=%u binding=%u）未找到",
                                   desc.debugName ? desc.debugName : "?", slot.glName, slot.set,
                                   slot.binding);
                    }
                    else
                    {
                        m_gl.Uniform1i(location, static_cast<GLint>(mapping.glSlot));
                    }
                }
                break;
            }
            }
            record.bindings.push_back(mapping);
        }

        m_gl.UseProgram(0);

        // 每管线一个 VAO：顶点属性格式由管线固定，缓冲区在 bindVertexBuffer
        // 时再挂进来。旧实现用「全局一个 VAO + 每次绘制重设全部属性」，
        // 属性数越多越慢，且状态泄漏到下一个管线。
        if (m_gl.GenVertexArrays)
        {
            m_gl.GenVertexArrays(1, &record.vao);
        }

        const PipelineHandle handle = m_pipelines.add(std::move(record));
        m_log.debug("[gl] Graphics pipeline ready: %s (program=%u attrs=%u bindings=%u)",  // 图形管线就绪
                    desc.debugName ? desc.debugName : "?", program, desc.attributeCount, desc.bindingCount);
        return handle;
    }

    PipelineHandle GlDevice::createComputePipeline(const ComputePipelineDesc& desc)
    {
        if (!m_caps.computeShaders)
        {
            m_log.error("[gl] createComputePipeline: 当前上下文不支持计算着色器");
            return PipelineHandle{};
        }
        const GlShaderRecord* cs = m_shaders.get(desc.computeShader);
        if (!cs)
        {
            m_log.error("[gl] createComputePipeline: 计算着色器句柄无效");
            return PipelineHandle{};
        }

        const GLuint csName = compileStage(GL_COMPUTE_SHADER, *cs, desc.debugName);
        if (!csName)
        {
            return PipelineHandle{};
        }

        const GLuint program = m_gl.CreateProgram();
        m_gl.AttachShader(program, csName);
        m_gl.LinkProgram(program);
        m_gl.DeleteShader(csName);

        GLint linked = 0;
        m_gl.GetProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked)
        {
            GLint length = 0;
            m_gl.GetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
            std::vector<char> log(static_cast<size_t>(length > 1 ? length : 1), '\0');
            if (length > 0)
            {
                m_gl.GetProgramInfoLog(program, length, nullptr, log.data());
            }
            m_log.error("[gl] 计算管线链接失败（%s）：%s", desc.debugName ? desc.debugName : "?",
                        log.data());
            m_gl.DeleteProgram(program);
            return PipelineHandle{};
        }

        GlPipelineRecord record{};
        record.program = program;
        record.isCompute = true;
        record.pushConstantBytes = desc.pushConstantBytes;

        uint32_t nextUboBinding = kPushConstantBinding + 1;
        uint32_t nextTextureUnit = 0;
        for (uint32_t i = 0; i < desc.bindingCount; ++i)
        {
            const BindingSlot& slot = desc.bindings[i];
            GlBindingMapping mapping{};
            mapping.set = slot.set;
            mapping.binding = slot.binding;
            mapping.type = slot.type;
            mapping.glSlot = (slot.type == BindingType::SampledTexture ||
                              slot.type == BindingType::StorageTexture)
                                 ? nextTextureUnit++
                                 : nextUboBinding++;
            record.bindings.push_back(mapping);
        }

        return m_pipelines.add(std::move(record));
    }

    void GlDevice::destroyPipeline(PipelineHandle pipeline)
    {
        GlPipelineRecord* record = m_pipelines.get(pipeline);
        if (!record)
        {
            if (pipeline.valid())
            {
                m_log.warn("[gl] destroyPipeline: 句柄已失效（重复销毁？）");
            }
            return;
        }
        if (record->program && m_gl.DeleteProgram)
        {
            m_gl.DeleteProgram(record->program);
        }
        if (record->vao && m_gl.DeleteVertexArrays)
        {
            m_gl.DeleteVertexArrays(1, &record->vao);
        }
        m_pipelines.remove(pipeline);
    }

    // ==================== 缓冲区 ====================

    BufferHandle GlDevice::createBuffer(const BufferDesc& desc)
    {
        if (desc.size == 0)
        {
            m_log.error("[gl] createBuffer: size 为 0");
            return BufferHandle{};
        }
        if (!m_gl.GenBuffers)
        {
            return BufferHandle{};
        }

        GlBufferRecord record{};
        record.desc = desc;
        record.target = toGlBufferTarget(desc.usage);

        m_gl.GenBuffers(1, &record.name);
        m_gl.BindBuffer(record.target, record.name);

        const bool wantPersistent =
            m_caps.persistentMapping &&
            (desc.access == MemoryAccess::CpuToGpu || desc.access == MemoryAccess::CpuToGpuCoherent);
        if (wantPersistent)
        {
            // glBufferStorage 的 flags 只接受**存储位**：DYNAMIC_STORAGE / MAP_READ /
            // MAP_WRITE / MAP_PERSISTENT / MAP_COHERENT / CLIENT_STORAGE。
            //
            // 这里绝不能塞 GL_MAP_FLUSH_EXPLICIT_BIT —— 那是 glMapBufferRange 的
            // **访问位**，映射时才有意义（见 mapBuffer 里的 access 组装）。混进来
            // 会让驱动报「<flags> has unknown bits set」并让整个 BufferStorage 失败，
            // 缓冲于是一个字节的存储都没有；之后所有 map/subData 都是
            // 「Invalid offset and/or size」，而绘制会从一块无存储的缓冲取顶点 ——
            // NVIDIA 上直接崩在驱动里，llvmpipe 上却能跑，极难定位。
            GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_DYNAMIC_STORAGE_BIT;
            if (desc.access == MemoryAccess::CpuToGpuCoherent)
            {
                flags |= GL_MAP_COHERENT_BIT;
            }
            m_gl.BufferStorage(record.target, static_cast<GLsizeiptr>(desc.size), nullptr, flags);
        }
        else
        {
            m_gl.BufferData(record.target, static_cast<GLsizeiptr>(desc.size), nullptr,
                            toGlBufferUsageHint(desc.access));
        }
        m_gl.BindBuffer(record.target, 0);

        return m_buffers.add(std::move(record));
    }

    void GlDevice::destroyBuffer(BufferHandle buffer)
    {
        GlBufferRecord* record = m_buffers.get(buffer);
        if (!record)
        {
            if (buffer.valid())
            {
                m_log.warn("[gl] destroyBuffer: 句柄已失效（重复销毁？）");
            }
            return;
        }
        if (record->mappedPtr && m_gl.UnmapBuffer)
        {
            // 销毁一个仍处于映射状态的缓冲在 GL 上是未定义行为，先解除映射
            m_gl.BindBuffer(record->target, record->name);
            m_gl.UnmapBuffer(record->target);
            m_gl.BindBuffer(record->target, 0);
        }
        if (record->name && m_gl.DeleteBuffers)
        {
            m_gl.DeleteBuffers(1, &record->name);
        }
        m_buffers.remove(buffer);
    }

    RhiResult GlDevice::writeBuffer(BufferHandle buffer, uint64_t offset, const void* data,
                                    uint64_t sizeBytes)
    {
        GlBufferRecord* record = m_buffers.get(buffer);
        if (!record || !data || sizeBytes == 0)
        {
            return RhiResult::ErrorInvalidArgument;
        }
        if (offset + sizeBytes > record->desc.size)
        {
            m_log.error("[gl] writeBuffer 越界：offset=%llu size=%llu 缓冲=%llu",
                        static_cast<unsigned long long>(offset),
                        static_cast<unsigned long long>(sizeBytes),
                        static_cast<unsigned long long>(record->desc.size));
            return RhiResult::ErrorInvalidArgument;
        }
        m_gl.BindBuffer(record->target, record->name);
        m_gl.BufferSubData(record->target, static_cast<GLintptr>(offset),
                           static_cast<GLsizeiptr>(sizeBytes), data);
        m_gl.BindBuffer(record->target, 0);
        return RhiResult::Ok;
    }

    MappedRange GlDevice::mapBuffer(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes)
    {
        GlBufferRecord* record = m_buffers.get(buffer);
        if (!record || !m_gl.MapBufferRange)
        {
            return MappedRange{};
        }
        if (record->desc.access == MemoryAccess::GpuOnly)
        {
            m_log.error("[gl] mapBuffer: GpuOnly 缓冲不可映射");
            return MappedRange{};
        }
        if (record->mappedPtr)
        {
            m_log.error("[gl] mapBuffer: 该缓冲已处于映射状态");
            return MappedRange{};
        }
        const uint64_t size = sizeBytes == 0 ? record->desc.size - offset : sizeBytes;
        if (offset + size > record->desc.size)
        {
            m_log.error("[gl] mapBuffer 越界");
            return MappedRange{};
        }

        GLbitfield access = GL_MAP_WRITE_BIT;
        if (record->desc.access == MemoryAccess::GpuToCpu)
        {
            access = GL_MAP_READ_BIT;
        }
        else if (m_caps.persistentMapping)
        {
            access |= GL_MAP_PERSISTENT_BIT;
            access |= (record->desc.access == MemoryAccess::CpuToGpuCoherent) ? GL_MAP_COHERENT_BIT
                                                                             : GL_MAP_FLUSH_EXPLICIT_BIT;
        }
        else
        {
            // 非持久映射：显式声明不同步，避免驱动为等待 GPU 而阻塞。
            // 调用方负责不覆盖仍在使用的区域（瞬态环形分配保证这一点）。
            access |= GL_MAP_UNSYNCHRONIZED_BIT;
        }

        m_gl.BindBuffer(record->target, record->name);
        void* ptr = m_gl.MapBufferRange(record->target, static_cast<GLintptr>(offset),
                                        static_cast<GLsizeiptr>(size), access);
        m_gl.BindBuffer(record->target, 0);
        if (!ptr)
        {
            m_log.error("[gl] mapBuffer 失败（GL 返回空指针）");
            return MappedRange{};
        }
        record->mappedPtr = ptr;
        return MappedRange{ ptr, offset, size };
    }

    void GlDevice::unmapBuffer(BufferHandle buffer)
    {
        GlBufferRecord* record = m_buffers.get(buffer);
        if (!record || !record->mappedPtr || !m_gl.UnmapBuffer)
        {
            return;
        }
        m_gl.BindBuffer(record->target, record->name);
        m_gl.UnmapBuffer(record->target);
        m_gl.BindBuffer(record->target, 0);
        record->mappedPtr = nullptr;
    }

    void GlDevice::flushMappedRange(BufferHandle buffer, uint64_t offset, uint64_t sizeBytes)
    {
        GlBufferRecord* record = m_buffers.get(buffer);
        if (!record || !record->mappedPtr || !m_gl.FlushMappedBufferRange)
        {
            return;
        }
        m_gl.BindBuffer(record->target, record->name);
        m_gl.FlushMappedBufferRange(record->target, static_cast<GLintptr>(offset),
                                    static_cast<GLsizeiptr>(sizeBytes));
        m_gl.BindBuffer(record->target, 0);
    }

    // ==================== 纹理与采样器 ====================

    TextureHandle GlDevice::createTexture(const TextureDesc& desc)
    {
        if (desc.width == 0 || desc.height == 0)
        {
            m_log.error("[gl] createTexture: 尺寸为 0");
            return TextureHandle{};
        }
        if (desc.width > m_caps.maxTextureSize || desc.height > m_caps.maxTextureSize)
        {
            m_log.error("[gl] createTexture: %ux%u 超过 maxTextureSize=%u", desc.width, desc.height,
                        m_caps.maxTextureSize);
            return TextureHandle{};
        }
        const GLenum internalFormat = toGlInternalFormat(desc.format);
        if (internalFormat == 0 || !m_gl.GenTextures)
        {
            m_log.error("[gl] createTexture: 不支持的格式 %d", static_cast<int>(desc.format));
            return TextureHandle{};
        }

        GlTextureRecord record{};
        record.desc = desc;
        m_gl.GenTextures(1, &record.name);
        m_gl.BindTexture(GL_TEXTURE_2D, record.name);
        m_gl.TexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(internalFormat),
                        static_cast<GLsizei>(desc.width), static_cast<GLsizei>(desc.height), 0,
                        toGlBaseFormat(desc.format), toGlPixelType(desc.format), nullptr);
        // 默认过滤/环绕：不设的话 GL 默认 MIPMAP_LINEAR，而我们只分配了 mip 0，
        // 采样结果会是黑色——这是一个经典的「纹理全黑」成因。
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        m_gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_gl.BindTexture(GL_TEXTURE_2D, 0);

        return m_textures.add(std::move(record));
    }

    void GlDevice::destroyTexture(TextureHandle texture)
    {
        GlTextureRecord* record = m_textures.get(texture);
        if (!record)
        {
            if (texture.valid())
            {
                m_log.warn("[gl] destroyTexture: 句柄已失效（重复销毁？）");
            }
            return;
        }
        if (record->name && m_gl.DeleteTextures)
        {
            m_gl.DeleteTextures(1, &record->name);
        }
        m_textures.remove(texture);
    }

    RhiResult GlDevice::writeTexture(TextureHandle texture, uint32_t mipLevel, const Rect2D& region,
                                     const void* data, uint64_t sizeBytes)
    {
        GlTextureRecord* record = m_textures.get(texture);
        if (!record || !data)
        {
            return RhiResult::ErrorInvalidArgument;
        }
        const uint32_t pixelSize = formatByteSize(record->desc.format);
        if (pixelSize == 0)
        {
            return RhiResult::ErrorUnsupported;
        }
        if (sizeBytes < static_cast<uint64_t>(region.width) * region.height * pixelSize)
        {
            m_log.error("[gl] writeTexture: 数据不足");
            return RhiResult::ErrorInvalidArgument;
        }

        m_gl.BindTexture(GL_TEXTURE_2D, record->name);
        if (m_gl.PixelStorei)
        {
            // 必须设为 1：默认 4 字节行对齐会让 R8 字体图集按行错位，
            // 表现为字形整体倾斜/撕裂。
            m_gl.PixelStorei(GL_UNPACK_ALIGNMENT, 1);
        }
        m_gl.TexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(mipLevel), region.x, region.y,
                           static_cast<GLsizei>(region.width), static_cast<GLsizei>(region.height),
                           toGlBaseFormat(record->desc.format), toGlPixelType(record->desc.format), data);
        m_gl.BindTexture(GL_TEXTURE_2D, 0);
        return RhiResult::Ok;
    }

    SamplerHandle GlDevice::createSampler(const SamplerDesc& desc)
    {
        // GL 3.3 的采样器对象（glGenSamplers）未纳入函数表，
        // 采样参数在 bindBindGroup 时设到纹理上。语义等价，只是无法
        // 让同一纹理在不同槽位用不同过滤方式——当前无此需求。
        return m_samplers.add(GlSamplerRecord{ desc });
    }

    void GlDevice::destroySampler(SamplerHandle sampler)
    {
        if (sampler.valid() && !m_samplers.remove(sampler))
        {
            m_log.warn("[gl] destroySampler: 句柄已失效（重复销毁？）");
        }
    }

    // ==================== 绑定组 ====================

    BindGroupHandle GlDevice::createBindGroup(const BindGroupDesc& desc)
    {
        GlBindGroupRecord record{};
        for (uint32_t i = 0; i < desc.bufferCount; ++i)
        {
            if (!m_buffers.get(desc.buffers[i].buffer))
            {
                m_log.error("[gl] createBindGroup: buffers[%u] 句柄无效", i);
                return BindGroupHandle{};
            }
            record.buffers.push_back(desc.buffers[i]);
        }
        for (uint32_t i = 0; i < desc.textureCount; ++i)
        {
            if (!m_textures.get(desc.textures[i].texture))
            {
                m_log.error("[gl] createBindGroup: textures[%u] 句柄无效", i);
                return BindGroupHandle{};
            }
            record.textures.push_back(desc.textures[i]);
        }
        return m_bindGroups.add(std::move(record));
    }

    void GlDevice::destroyBindGroup(BindGroupHandle group)
    {
        if (group.valid() && !m_bindGroups.remove(group))
        {
            m_log.warn("[gl] destroyBindGroup: 句柄已失效（重复销毁？）");
        }
    }

    // ==================== 帧 ====================

    ICommandList* GlDevice::beginFrame(ISurface* surface)
    {
        if (!surface)
        {
            m_log.error("[gl] beginFrame: surface 为空");
            return nullptr;
        }
        if (m_inFrame)
        {
            m_log.error("[gl] beginFrame: 上一帧未 submitFrame");
            return nullptr;
        }

        auto* glSurface = static_cast<GlSurface*>(surface);
        bool owned = false;
        for (GlSurface* s : m_surfaces)
        {
            owned = owned || (s == glSurface);
        }
        if (!owned)
        {
            m_log.error("[gl] beginFrame: 表面不属于本设备");
            return nullptr;
        }

        m_inFrame = true;
        m_frameSurface = glSurface;
        m_commands.beginFrame(glSurface);
        return &m_commands;
    }

    RhiResult GlDevice::submitFrame()
    {
        if (!m_inFrame)
        {
            m_log.error("[gl] submitFrame: 未先调用 beginFrame");
            return RhiResult::ErrorInvalidArgument;
        }
        if (m_commands.inRenderPass())
        {
            m_log.error("[gl] submitFrame: RenderPass 未结束");
            return RhiResult::ErrorInvalidArgument;
        }

        // GL 的提交是隐式的：命令在调用时就进了驱动队列。
        // 这里只做「解绑 + 计数」，present 由 ISurface 负责——
        // 提交与呈现分离是多窗口共享设备的前提。
        if (m_gl.BindVertexArray)
        {
            m_gl.BindVertexArray(0);
        }
        if (m_gl.UseProgram)
        {
            m_gl.UseProgram(0);
        }

        m_inFrame = false;
        m_frameSurface = nullptr;
        m_frameIndex += 1;
        return RhiResult::Ok;
    }

    void GlDevice::waitIdle()
    {
        if (!m_gl.FenceSync || !m_gl.ClientWaitSync || !m_gl.DeleteSync)
        {
            if (m_gl.Flush)
            {
                m_gl.Flush();
            }
            return;
        }
        // glFinish 不在函数表里；用 fence + ClientWaitSync 等价且更可控
        GLsync sync = m_gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        if (!sync)
        {
            return;
        }
        m_gl.ClientWaitSync(sync, GL_SYNC_FLUSH_COMMANDS_BIT, ~0ull);
        m_gl.DeleteSync(sync);
    }

    // ==================== 帧缓冲缓存与读回 ====================

    GLuint GlDevice::framebufferFor(const RenderPassBeginDesc& desc)
    {
        GLuint colorNames[kMaxColorAttachments]{};
        uint32_t colorCount = 0;
        for (uint32_t i = 0; i < desc.colorAttachmentCount && i < kMaxColorAttachments; ++i)
        {
            GlTextureRecord* record = m_textures.get(desc.colorAttachments[i].texture);
            colorNames[colorCount++] = record ? record->name : 0;
        }
        GLuint depthName = 0;
        if (desc.depthAttachment.texture.valid())
        {
            GlTextureRecord* record = m_textures.get(desc.depthAttachment.texture);
            depthName = record ? record->name : 0;
        }

        for (const auto& entry : m_framebufferCache)
        {
            if (entry.colorCount != colorCount || entry.depthName != depthName)
            {
                continue;
            }
            bool same = true;
            for (uint32_t i = 0; i < colorCount; ++i)
            {
                same = same && entry.colorNames[i] == colorNames[i];
            }
            if (same)
            {
                return entry.fbo;
            }
        }

        if (!m_gl.GenFramebuffers)
        {
            return 0;
        }

        FramebufferCacheEntry entry{};
        entry.colorCount = colorCount;
        entry.depthName = depthName;
        for (uint32_t i = 0; i < colorCount; ++i)
        {
            entry.colorNames[i] = colorNames[i];
        }

        m_gl.GenFramebuffers(1, &entry.fbo);
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, entry.fbo);
        for (uint32_t i = 0; i < colorCount; ++i)
        {
            m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D,
                                      entry.colorNames[i], 0);
        }
        if (depthName)
        {
            m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depthName, 0);
        }

        if (m_gl.CheckFramebufferStatus)
        {
            const GLenum status = m_gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
            if (status != GL_FRAMEBUFFER_COMPLETE)
            {
                m_log.error("[gl] 帧缓冲不完整（status=0x%04X，color=%u depth=%u）", status, colorCount,
                            depthName);
                m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
                m_gl.DeleteFramebuffers(1, &entry.fbo);
                return 0;
            }
        }
        m_gl.BindFramebuffer(GL_FRAMEBUFFER, 0);

        m_framebufferCache.push_back(entry);
        return entry.fbo;
    }

    RhiResult GlDevice::readTexture(TextureHandle texture, const Rect2D& region, void* outPixels,
                                    uint64_t bufferSize, uint32_t* outRowPitch)
    {
        GlTextureRecord* record = m_textures.get(texture);
        if (!record || !outPixels || region.width == 0 || region.height == 0)
        {
            return RhiResult::ErrorInvalidArgument;
        }
        if (!m_gl.GenFramebuffers || !m_gl.ReadPixels)
        {
            return RhiResult::ErrorUnsupported;
        }
        const uint32_t pixelSize = formatByteSize(record->desc.format);
        const uint32_t rowPitch = region.width * pixelSize;
        if (bufferSize < static_cast<uint64_t>(rowPitch) * region.height)
        {
            return RhiResult::ErrorInvalidArgument;
        }

        if (!m_readbackFbo)
        {
            m_gl.GenFramebuffers(1, &m_readbackFbo);
        }

        GLint previousFbo = 0;
        if (m_gl.GetIntegerv)
        {
            m_gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFbo);
        }

        m_gl.BindFramebuffer(GL_FRAMEBUFFER, m_readbackFbo);
        m_gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, record->name, 0);

        if (m_gl.PixelStorei)
        {
            m_gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
        }

        // GL 的读回原点在左下角，而 RHI 契约统一为左上角。
        // 逐行反向读取，翻转在这里完成，不把 GL 的坐标习惯泄漏给调用方。
        auto* dst = static_cast<uint8_t*>(outPixels);
        const int32_t texHeight = static_cast<int32_t>(record->desc.height);
        for (uint32_t row = 0; row < region.height; ++row)
        {
            const int32_t glY = texHeight - (region.y + static_cast<int32_t>(row)) - 1;
            m_gl.ReadPixels(region.x, glY, static_cast<GLsizei>(region.width), 1,
                            toGlBaseFormat(record->desc.format), toGlPixelType(record->desc.format),
                            dst + static_cast<size_t>(row) * rowPitch);
        }

        m_gl.BindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFbo < 0 ? 0 : previousFbo));

        if (outRowPitch)
        {
            *outRowPitch = rowPitch;
        }
        return RhiResult::Ok;
    }

    uint64_t GlDevice::gpuMemoryUsageBytes() const
    {
        uint64_t total = 0;
        auto* self = const_cast<GlDevice*>(this);
        for (const auto& buffer : self->m_buffers)
        {
            total += buffer.desc.size;
        }
        for (const auto& texture : self->m_textures)
        {
            total += static_cast<uint64_t>(texture.desc.width) * texture.desc.height *
                     formatByteSize(texture.desc.format);
        }
        return total;
    }

    void GlDevice::deleteAllResources()
    {
        // 泄漏是宿主的生命周期错误，但设备销毁时必须把 GPU 对象还给驱动，
        // 否则上下文销毁前这些对象一直占显存。数量非 0 时记一条 Warn，
        // 让问题可见而不是被静默吞掉。
        uint32_t leaked = m_buffers.size() + m_textures.size() + m_pipelines.size();
        if (leaked > 0)
        {
            m_log.warn("[gl] 设备销毁时仍有 %u 个资源未释放（buffers=%u textures=%u pipelines=%u）",
                       leaked, m_buffers.size(), m_textures.size(), m_pipelines.size());
        }

        for (auto& buffer : m_buffers)
        {
            if (buffer.name && m_gl.DeleteBuffers)
            {
                m_gl.DeleteBuffers(1, &buffer.name);
            }
        }
        for (auto& texture : m_textures)
        {
            if (texture.name && m_gl.DeleteTextures)
            {
                m_gl.DeleteTextures(1, &texture.name);
            }
        }
        for (auto& pipeline : m_pipelines)
        {
            if (pipeline.program && m_gl.DeleteProgram)
            {
                m_gl.DeleteProgram(pipeline.program);
            }
            if (pipeline.vao && m_gl.DeleteVertexArrays)
            {
                m_gl.DeleteVertexArrays(1, &pipeline.vao);
            }
        }

        m_buffers.clear();
        m_textures.clear();
        m_samplers.clear();
        m_shaders.clear();
        m_pipelines.clear();
        m_bindGroups.clear();
    }

}  // namespace Render::RHI::gl

namespace Render::RHI
{

    IGpuDevice* createGlDevice(const DeviceDesc& desc)
    {
        RhiLogger logger(desc.logCallback, desc.logUserData);

        // GL 后端要求调用时已有当前上下文：函数指针必须在上下文内解析。
        // 这是 ForeignGlContext 集成方式的硬约束，宿主应在 initializeGL 内调用。
        GLFuncs functions{};
        if (!gl_loader_load(&functions, desc.glGetProcAddress))
        {
            logger.error("[gl] createDevice 失败：GL 函数加载失败。"
                         "请确认调用时已有当前 GL 上下文（Qt 应在 initializeGL 内创建设备）");
            return nullptr;
        }

        return new gl::GlDevice(desc, functions);
    }

}  // namespace Render::RHI
