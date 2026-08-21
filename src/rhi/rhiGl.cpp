#include "rhiGl.h"
#include "platform/glLoader.h"
#include "../shader/shaders.h"
#include "Log/SyLogger.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#ifdef _WIN32
    #include <GL/gl.h>
#elif defined(__linux__)
    #include <GL/glx.h>
#endif

namespace Render::RHI
{
    GLDevice::~GLDevice()
    {
        if (m_initialized)
        {
            shutdown();
        }
    }

    bool GLDevice::initialize(void* nativeWindow, uint32_t width, uint32_t height)
    {
        m_nativeContext = nativeWindow;
        m_width = width;
        m_height = height;

        if (!gl_loader_init(nullptr))
        {
            std::fprintf(stderr, "[rhiGl] gl_loader_init failed\n");
            return false;
        }

        auto* g = gl();

        if (g->CreateVertexArrays)
        {
            g->CreateVertexArrays(1, &m_vao);
        }
        else
        {
            g->GenVertexArrays(1, &m_vao);
        }
        if (g->BindVertexArray)
        {
            g->BindVertexArray(m_vao);
        }

        g->Enable(GL_MULTISAMPLE);
        // 修复：不再全局启用 GL_LINE_SMOOTH。
        // 原因：GL_LINE_SMOOTH 与 4x MSAA 叠加时，基于覆盖率的抗锯齿算法
        // 会在某些缩放级别下丢弃低于覆盖阈值的线段片段，导致缩放后线条消失。
        // MSAA 已提供足够的多重采样抗锯齿，无需额外的 GL_LINE_SMOOTH。
        // 如需启用，应配合 glHint(GL_LINE_SMOOTH_HINT, GL_NICEST) 并仅在非 MSAA 环境下使用。

        // 查询 GPU 支持的线宽范围
        // macOS CoreProfile 下 GL_LINE_WIDTH_RANGE 通常为 [1, 1]（仅支持 1px 线宽）
        // Windows CompatibilityProfile 下可能支持更宽的线宽
        if (g->GetFloatv)
        {
            float range[2] = { 1.0f, 1.0f };
            g->GetFloatv(GL_LINE_WIDTH_RANGE, range);
            m_minLineWidth = range[0] > 0.0f ? range[0] : 1.0f;
            m_maxLineWidth = range[1] >= m_minLineWidth ? range[1] : m_minLineWidth;
        }

        m_initialized = true;
        return true;
    }

    void GLDevice::shutdown()
    {
        if (!m_initialized)
        {
            return;
        }

        auto* g = gl();

        for (auto& entry : m_buffers)
        {
            if (entry.glName)
            {
                g->DeleteBuffers(1, &entry.glName);
                entry.glName = 0;
            }
        }
        m_buffers.clear();
        m_bufferFreeList.clear();

        for (auto& entry : m_textures)
        {
            if (entry.glName)
            {
                g->DeleteTextures(1, &entry.glName);
                entry.glName = 0;
            }
        }
        m_textures.clear();
        m_textureFreeList.clear();

        for (auto& entry : m_pipelines)
        {
            if (entry.program)
            {
                g->DeleteProgram(entry.program);
                entry.program = 0;
            }
        }
        m_pipelines.clear();
        m_pipelineFreeList.clear();

        if (m_vao)
        {
            g->DeleteVertexArrays(1, &m_vao);
            m_vao = 0;
        }

        m_initialized = false;
    }

    BufferHandle GLDevice::allocBufferHandle()
    {
        if (!m_bufferFreeList.empty())
        {
            uint32_t idx = m_bufferFreeList.back();
            m_bufferFreeList.pop_back();
            return BufferHandle(idx + 1);
        }
        m_buffers.emplace_back();
        return BufferHandle(m_buffers.size());
    }

    TextureHandle GLDevice::allocTextureHandle()
    {
        if (!m_textureFreeList.empty())
        {
            uint32_t idx = m_textureFreeList.back();
            m_textureFreeList.pop_back();
            return TextureHandle(idx + 1);
        }
        m_textures.emplace_back();
        return TextureHandle(m_textures.size());
    }

    PipelineHandle GLDevice::allocPipelineHandle()
    {
        if (!m_pipelineFreeList.empty())
        {
            uint32_t idx = m_pipelineFreeList.back();
            m_pipelineFreeList.pop_back();
            return PipelineHandle(idx + 1);
        }
        m_pipelines.emplace_back();
        return PipelineHandle(m_pipelines.size());
    }

    BufferHandle GLDevice::createBuffer(const BufferDesc& desc)
    {
        auto* g = gl();
        BufferHandle handle = allocBufferHandle();
        auto& entry = m_buffers[size_t(handle - 1)];

        entry.size = desc.size;
        entry.usage = desc.usage;
        entry.memory = desc.memory;
        entry.mappedPtr = nullptr;

        if (g->CreateBuffers)
        {
            g->CreateBuffers(1, &entry.glName);
            // 对于需要 CPU 持续写入的缓冲区，使用不可变存储配合持久映射标志
            if (desc.memory == MemoryType::GPU_CPU_Coherent && g->NamedBufferStorage)
            {
                // 注意：必须包含 GL_MAP_READ_BIT，否则后续 mapBuffer 用 GL_MAP_READ_BIT
                // 读取该缓冲（如 GPU 剔除回读 count/visibility buffer）会失败返回 nullptr，
                // 导致 GPU 剔除结果永远为 0，只能回退 CPU 四叉树。
                GLbitfield flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                    GL_MAP_COHERENT_BIT;
                g->NamedBufferStorage(entry.glName, (GLsizeiptr)desc.size, nullptr, flags);
            }
            else
            {
                g->NamedBufferData(entry.glName, (GLsizeiptr)desc.size, nullptr, GL_DYNAMIC_DRAW);
            }
        }
        else
        {
            g->GenBuffers(1, &entry.glName);
            g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
            if (desc.memory == MemoryType::GPU_CPU_Coherent && g->BufferStorage)
            {
                GLbitfield flags = GL_DYNAMIC_STORAGE_BIT | GL_MAP_READ_BIT | GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT |
                    GL_MAP_COHERENT_BIT;
                g->BufferStorage(GL_ARRAY_BUFFER, (GLsizeiptr)desc.size, nullptr, flags);
            }
            else
            {
                g->BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)desc.size, nullptr, GL_DYNAMIC_DRAW);
            }
            g->BindBuffer(GL_ARRAY_BUFFER, 0);
        }

        return handle;
    }

    void GLDevice::destroyBuffer(BufferHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto idx = size_t(handle - 1);
        if (idx >= m_buffers.size())
        {
            return;
        }

        auto* g = gl();
        auto& entry = m_buffers[idx];
        if (entry.glName)
        {
            g->DeleteBuffers(1, &entry.glName);
        }
        entry = GLBufferEntry{};
        m_bufferFreeList.push_back(uint32_t(idx));
    }

    TextureHandle GLDevice::createTexture(const TextureDesc& desc)
    {
        auto* g = gl();
        TextureHandle handle = allocTextureHandle();
        auto& entry = m_textures[size_t(handle - 1)];

        entry.format = desc.format;
        entry.width = desc.width;
        entry.height = desc.height;
        entry.mipLevels = desc.mipLevels;

        if (g->CreateTextures)
        {
            g->CreateTextures(GL_TEXTURE_2D, 1, &entry.glName);
            g->TextureStorage2D(entry.glName, desc.mipLevels, formatToGLInternal(desc.format), desc.width, desc.height);
            g->TextureParameteri(entry.glName, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g->TextureParameteri(entry.glName, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g->TextureParameteri(entry.glName, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->TextureParameteri(entry.glName, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        else
        {
            g->GenTextures(1, &entry.glName);
            g->BindTexture(GL_TEXTURE_2D, entry.glName);
            g->TexImage2D(GL_TEXTURE_2D,
                0,
                formatToGLInternal(desc.format),
                desc.width,
                desc.height,
                0,
                formatToGLFormat(desc.format),
                formatToGLType(desc.format),
                nullptr);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g->BindTexture(GL_TEXTURE_2D, 0);
        }

        return handle;
    }

    void GLDevice::destroyTexture(TextureHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto idx = size_t(handle - 1);
        if (idx >= m_textures.size())
        {
            return;
        }

        auto* g = gl();
        auto& entry = m_textures[idx];
        if (entry.glName)
        {
            g->DeleteTextures(1, &entry.glName);
        }
        entry = GLTextureEntry{};
        m_textureFreeList.push_back(uint32_t(idx));
    }

    PipelineHandle GLDevice::createPipeline(const PipelineDesc& desc)
    {
        auto* g = gl();
        PipelineHandle handle = allocPipelineHandle();
        auto& entry = m_pipelines[size_t(handle - 1)];

        // compute pipeline 分支：只需要 compute shader
        if (desc.computeShader)
        {
            const char* csSource = desc.computeShader;

            // compute shader 名称映射
            if (std::strcmp(csSource, "culling_comp") == 0)
            {
                csSource = shader::CULLING_COMP;
            }

            uint32_t cs = compileShader(GL_COMPUTE_SHADER, csSource);
            if (!cs)
            {
                entry = GLPipelineEntry{};
                m_pipelineFreeList.push_back(uint32_t(handle - 1));
                return NullHandle;
            }

            uint32_t prog = g->CreateProgram();
            g->AttachShader(prog, cs);
            g->LinkProgram(prog);
            g->DeleteShader(cs);

            GLint success = 0;
            g->GetProgramiv(prog, GL_LINK_STATUS, &success);
            if (!success)
            {
                GLint logLen = 0;
                g->GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
                if (logLen > 0)
                {
                    char* log = new char[logLen];
                    g->GetProgramInfoLog(prog, logLen, nullptr, log);
                    SY_ERRORF("[rhiGl] Compute pipeline link error:\n%s", log);
                    delete[] log;
                }
                g->DeleteProgram(prog);
                entry = GLPipelineEntry{};
                m_pipelineFreeList.push_back(uint32_t(handle - 1));
                return NullHandle;
            }

            entry.program = prog;
            entry.topology = RHI::PrimitiveTopology::PointList;  // compute pipeline 不使用
            entry.depthTest = false;
            entry.depthWrite = false;
            entry.blendEnable = false;

            // 预注册 compute shader 的 uniform 位置
            static const std::vector<std::string> computeUniforms = { "uViewProjMatrix", "uEntityCount", "uModelMatrix" };
            for (const auto& name : computeUniforms)
            {
                GLint loc = g->GetUniformLocation(prog, name.c_str());
                if (loc >= 0)
                {
                    entry.uniformLocations[name] = loc;
                }
            }

            SY_DEBUGF("[rhiGl] compute pipeline created: program=%u", prog);
            return handle;
        }

        // 图形 pipeline 分支：需要 vertex + fragment shader
        const char* vsSource = desc.vertexShader;
        const char* fsSource = desc.fragmentShader;

        if (strcmp(desc.vertexShader, "passthrough_vert") == 0)
        {
            vsSource = shader::SCENE_2D_VERT;
        }
        else if (strcmp(desc.vertexShader, "passthrough_frag") == 0)
        {
            vsSource = shader::SCENE_2D_FRAG;
        }
        else if (strcmp(desc.vertexShader, "overlay_vert") == 0)
        {
            vsSource = shader::OVERLAY_VERT;
        }
        else if (strcmp(desc.vertexShader, "overlay_frag") == 0)
        {
            vsSource = shader::OVERLAY_FRAG;
        }
        else if (strcmp(desc.vertexShader, "overlay_screen_vert") == 0)
        {
            vsSource = shader::OVERLAY_SCREEN_VERT;
        }
        else if (strcmp(desc.vertexShader, "overlay_screen_frag") == 0)
        {
            vsSource = shader::OVERLAY_SCREEN_FRAG;
        }
        else if (strcmp(desc.vertexShader, "mesh_3d_vert") == 0)
        {
            vsSource = shader::MESH_3D_VERT;
        }
        else if (strcmp(desc.vertexShader, "mesh_3d_frag") == 0)
        {
            vsSource = shader::MESH_3D_FRAG;
        }
        else if (strcmp(desc.vertexShader, "mesh_3d_instanced_vert") == 0)
        {
            vsSource = shader::MESH_3D_INSTANCED_VERT;
        }
        else if (strcmp(desc.vertexShader, "text_sdf_vert") == 0)
        {
            vsSource = shader::TEXT_SDF_VERT;
        }
        else if (strcmp(desc.vertexShader, "text_sdf_frag") == 0)
        {
            vsSource = shader::TEXT_SDF_FRAG;
        }
        else if (strcmp(desc.vertexShader, "bitmap_vert") == 0)
        {
            vsSource = shader::BITMAP_VERT;
        }
        else if (strcmp(desc.vertexShader, "bitmap_frag") == 0)
        {
            vsSource = shader::BITMAP_FRAG;
        }
        else if (strcmp(desc.vertexShader, "highlight_3d_vert") == 0)
        {
            vsSource = shader::HIGHLIGHT_3D_VERT;
        }
        else if (strcmp(desc.vertexShader, "highlight_3d_frag") == 0)
        {
            vsSource = shader::HIGHLIGHT_3D_FRAG;
        }

        if (strcmp(desc.fragmentShader, "passthrough_vert") == 0)
        {
            fsSource = shader::SCENE_2D_VERT;
        }
        else if (strcmp(desc.fragmentShader, "passthrough_frag") == 0)
        {
            fsSource = shader::SCENE_2D_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "overlay_vert") == 0)
        {
            fsSource = shader::OVERLAY_VERT;
        }
        else if (strcmp(desc.fragmentShader, "overlay_frag") == 0)
        {
            fsSource = shader::OVERLAY_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "overlay_screen_vert") == 0)
        {
            fsSource = shader::OVERLAY_SCREEN_VERT;
        }
        else if (strcmp(desc.fragmentShader, "overlay_screen_frag") == 0)
        {
            fsSource = shader::OVERLAY_SCREEN_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "mesh_3d_vert") == 0)
        {
            fsSource = shader::MESH_3D_VERT;
        }
        else if (strcmp(desc.fragmentShader, "mesh_3d_frag") == 0)
        {
            fsSource = shader::MESH_3D_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "mesh_3d_instanced_vert") == 0)
        {
            fsSource = shader::MESH_3D_INSTANCED_VERT;
        }
        else if (strcmp(desc.fragmentShader, "text_sdf_vert") == 0)
        {
            fsSource = shader::TEXT_SDF_VERT;
        }
        else if (strcmp(desc.fragmentShader, "text_sdf_frag") == 0)
        {
            fsSource = shader::TEXT_SDF_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "bitmap_vert") == 0)
        {
            fsSource = shader::BITMAP_VERT;
        }
        else if (strcmp(desc.fragmentShader, "bitmap_frag") == 0)
        {
            fsSource = shader::BITMAP_FRAG;
        }
        else if (strcmp(desc.fragmentShader, "highlight_3d_vert") == 0)
        {
            fsSource = shader::HIGHLIGHT_3D_VERT;
        }
        else if (strcmp(desc.fragmentShader, "highlight_3d_frag") == 0)
        {
            fsSource = shader::HIGHLIGHT_3D_FRAG;
        }

        uint32_t vs = compileShader(GL_VERTEX_SHADER, vsSource);
        uint32_t fs = compileShader(GL_FRAGMENT_SHADER, fsSource);
        if (!vs || !fs)
        {
            if (vs)
            {
                g->DeleteShader(vs);
            }
            if (fs)
            {
                g->DeleteShader(fs);
            }
            entry = GLPipelineEntry{};
            m_pipelineFreeList.push_back(uint32_t(handle - 1));
            return NullHandle;
        }

        uint32_t prog = linkProgram(vs, fs);
        g->DeleteShader(vs);
        g->DeleteShader(fs);

        if (!prog)
        {
            entry = GLPipelineEntry{};
            m_pipelineFreeList.push_back(uint32_t(handle - 1));
            return NullHandle;
        }

        entry.program = prog;
        entry.topology = desc.topology;
        entry.vertexFormat = desc.vertexFormat;
        entry.depthTest = desc.depthTest;
        entry.depthWrite = desc.depthWrite;
        entry.blendEnable = desc.blendEnable;
        entry.srcBlend = desc.srcBlend;
        entry.dstBlend = desc.dstBlend;
        entry.depthFunc = desc.depthFunc;

        // ---- 诊断：打印管线 program 与关键 uniform location ----
        {
            GLint locVM = g->GetUniformLocation(prog, "uViewMatrix");
            GLint locCC = g->GetUniformLocation(prog, "uCameraCenter");

            auto shortName = [](const char* s) -> const char* {
                if (!s)
                    return "null";
                if (std::strlen(s) <= 32)
                    return s;
                static thread_local char buf[48];
                std::snprintf(buf, sizeof(buf), "%.32s...", s);
                return buf;
            };

            SY_DEBUGF(
                "[Pipeline] handle=%llu program=%u vs=%s fs=%s fmt=%d topo=%u uViewMatrixLoc=%d uCameraCenterLoc=%d",
                static_cast<unsigned long long>(handle),
                prog,
                shortName(desc.vertexShader),
                shortName(desc.fragmentShader),
                static_cast<int>(desc.vertexFormat),
                static_cast<uint32_t>(desc.topology),
                locVM,
                locCC);
        }

        static const std::vector<std::string> commonUniforms = { "uViewMatrix",
            "uProjMatrix",
            "uModelMatrix",
            "uViewportSize",
            "uFontAtlas",
            "uTexture",
            "uLightDir",
            "uAmbientColor",
            "uDiffuseColor",
            "uSpecularColor",
            "uShininess",
            "uHighlightColor" };
        for (const auto& name : commonUniforms)
        {
            GLint loc = g->GetUniformLocation(prog, name.c_str());
            if (loc >= 0)
            {
                entry.uniformLocations[name] = loc;
            }
        }

        return handle;
    }

    void GLDevice::destroyPipeline(PipelineHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto idx = size_t(handle - 1);
        if (idx >= m_pipelines.size())
        {
            return;
        }

        auto* g = gl();
        auto& entry = m_pipelines[idx];
        if (entry.program)
        {
            g->DeleteProgram(entry.program);
        }
        entry = GLPipelineEntry{};
        m_pipelineFreeList.push_back(uint32_t(idx));
    }

    void GLDevice::uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t size, const void* data)
    {
        if (handle == NullHandle || !data)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        if (g->NamedBufferSubData)
        {
            g->NamedBufferSubData(entry.glName, (GLintptr)offset, (GLsizeiptr)size, data);
        }
        else
        {
            GLenum target = GL_ARRAY_BUFFER;
            g->BindBuffer(target, entry.glName);
            g->BufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
            g->BindBuffer(target, 0);
        }
    }

    void GLDevice::uploadTexture(TextureHandle handle, uint32_t mip, const void* data, uint32_t rowPitch)
    {
        if (handle == NullHandle || !data)
        {
            return;
        }
        auto& entry = m_textures[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        uint32_t fmt = formatToGLFormat(entry.format);
        uint32_t type = formatToGLType(entry.format);

        if (rowPitch == 0)
        {
            rowPitch = entry.width * getFormatBytesPerPixel(entry.format);
        }

        if (g->TextureSubImage2D)
        {
            g->TextureSubImage2D(entry.glName,
                (GLint)mip,
                0,
                0,
                entry.width,
                entry.height,
                fmt,
                type,
                (const void*)((const char*)data + rowPitch * mip));
        }
        else
        {
            g->BindTexture(GL_TEXTURE_2D, entry.glName);
            g->TexSubImage2D(GL_TEXTURE_2D,
                (GLint)mip,
                0,
                0,
                entry.width,
                entry.height,
                fmt,
                type,
                (const void*)((const char*)data + rowPitch * mip));
            g->BindTexture(GL_TEXTURE_2D, 0);
        }
    }

    void* GLDevice::mapBuffer(BufferHandle handle, uint64_t offset, uint64_t size, uint32_t mapFlags)
    {
        if (handle == NullHandle)
        {
            return nullptr;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return nullptr;
        }

        auto* g = gl();
        GLbitfield glFlags = 0;
        if (mapFlags & GL_MAP_READ_BIT)
        {
            glFlags |= GL_MAP_READ_BIT;
        }
        if (mapFlags & GL_MAP_WRITE_BIT)
        {
            glFlags |= GL_MAP_WRITE_BIT;
        }
        if (mapFlags & GL_MAP_INVALIDATE_RANGE_BIT)
        {
            glFlags |= GL_MAP_INVALIDATE_RANGE_BIT;
        }
        if (mapFlags & GL_MAP_INVALIDATE_BUFFER_BIT)
        {
            glFlags |= GL_MAP_INVALIDATE_BUFFER_BIT;
        }
        if (mapFlags & GL_MAP_FLUSH_EXPLICIT_BIT)
        {
            glFlags |= GL_MAP_FLUSH_EXPLICIT_BIT;
        }
        if (mapFlags & GL_MAP_UNSYNCHRONIZED_BIT)
        {
            glFlags |= GL_MAP_UNSYNCHRONIZED_BIT;
        }
        if (mapFlags & GL_MAP_PERSISTENT_BIT)
        {
            glFlags |= GL_MAP_PERSISTENT_BIT;
        }
        if (mapFlags & GL_MAP_COHERENT_BIT)
        {
            glFlags |= GL_MAP_COHERENT_BIT;
        }

        void* ptr = nullptr;
        if (g->MapNamedBufferRange)
        {
            ptr = g->MapNamedBufferRange(entry.glName, (GLintptr)offset, (GLsizeiptr)size, glFlags);
        }
        else
        {
            g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
            ptr = g->MapBufferRange(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, glFlags);
            g->BindBuffer(GL_ARRAY_BUFFER, 0);
        }

        entry.mappedPtr = ptr;
        return ptr;
    }

    void GLDevice::unmapBuffer(BufferHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName || !entry.mappedPtr)
        {
            return;
        }

        auto* g = gl();
        if (g->UnmapNamedBuffer)
        {
            g->UnmapNamedBuffer(entry.glName);
        }
        else
        {
            g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
            g->UnmapBuffer(GL_ARRAY_BUFFER);
            g->BindBuffer(GL_ARRAY_BUFFER, 0);
        }
        entry.mappedPtr = nullptr;
    }

    void GLDevice::flushMappedRange(BufferHandle handle, uint64_t offset, uint64_t size)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName || !entry.mappedPtr)
        {
            return;
        }

        auto* g = gl();
        if (g->FlushMappedNamedBufferRange)
        {
            g->FlushMappedNamedBufferRange(entry.glName, (GLintptr)offset, (GLsizeiptr)size);
        }
        else
        {
            g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
            g->FlushMappedBufferRange(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size);
            g->BindBuffer(GL_ARRAY_BUFFER, 0);
        }
    }

    void GLDevice::beginFrame()
    {
        auto* g = gl();
        g->BindVertexArray(m_vao);
        g->Viewport(0, 0, (GLsizei)m_width, (GLsizei)m_height);
        g->ClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
        g->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        m_currentPipeline = NullHandle;
        for (int i = 0; i < 4; i++)
        {
            m_currentVBOs[i] = NullHandle;
            m_currentVBOOffsets[i] = 0;
        }
        m_currentIBO = NullHandle;
        m_currentIBOOffset = 0;
        m_depthTestEnabled = false;
        m_blendEnabled = false;
    }

    void GLDevice::endFrame()
    {
        auto* g = gl();
        g->Flush();
        m_completedFence++;
    }

    void GLDevice::present() {}

    bool GLDevice::checkFence(uint64_t fenceValue) const
    {
        return fenceValue <= m_completedFence;
    }

    void GLDevice::bindPipeline(PipelineHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(handle - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        g->UseProgram(entry.program);
        m_currentPipeline = handle;

        if (entry.depthTest)
        {
            g->Enable(GL_DEPTH_TEST);
            m_depthTestEnabled = true;
            uint32_t depthFuncGL = GL_LESS;
            switch (entry.depthFunc)
            {
            case CompareFunc::Never:
                depthFuncGL = 0x0200;
                break;
            case CompareFunc::Less:
                depthFuncGL = GL_LESS;
                break;
            case CompareFunc::Equal:
                depthFuncGL = 0x0202;
                break;
            case CompareFunc::LessEqual:
                depthFuncGL = GL_LEQUAL;
                break;
            case CompareFunc::Greater:
                depthFuncGL = 0x0204;
                break;
            case CompareFunc::Always:
                depthFuncGL = 0x0207;
                break;
            }
            g->DepthFunc(depthFuncGL);
        }
        else
        {
            g->Disable(GL_DEPTH_TEST);
            m_depthTestEnabled = false;
        }

        g->DepthMask(entry.depthWrite ? GL_TRUE : GL_FALSE);

        if (entry.blendEnable)
        {
            g->Enable(GL_BLEND);
            m_blendEnabled = true;
            auto toGLBlend = [](BlendFactor f) -> uint32_t {
                switch (f)
                {
                case BlendFactor::Zero:
                    return 0x0000;
                case BlendFactor::One:
                    return 0x0001;
                case BlendFactor::SrcAlpha:
                    return GL_SRC_ALPHA;
                case BlendFactor::OneMinusSrcAlpha:
                    return GL_ONE_MINUS_SRC_ALPHA;
                }
                return 0x0000;
            };
            g->BlendFunc(toGLBlend(entry.srcBlend), toGLBlend(entry.dstBlend));
        }
        else
        {
            g->Disable(GL_BLEND);
            m_blendEnabled = false;
        }
    }

    void GLDevice::bindVertexBuffer(uint32_t slot, BufferHandle handle, uint64_t offset)
    {
        if (handle == NullHandle || slot >= 4)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        // 根据当前管线的顶点格式计算跨度，避免 stride=0 被误算为单个属性大小
        GLsizei stride = 0;
        if (m_currentPipeline != NullHandle)
        {
            VertexFormat fmt = m_pipelines[size_t(m_currentPipeline - 1)].vertexFormat;
            stride = static_cast<GLsizei>(vertexFormatStride(fmt));
        }

        if (g->VertexArrayVertexBuffer)
        {
            g->VertexArrayVertexBuffer(m_vao, slot, entry.glName, (GLintptr)offset, stride);
        }
        else
        {
            g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
        }

        m_currentVBOs[slot] = handle;
        m_currentVBOOffsets[slot] = offset;

        if (m_currentPipeline != NullHandle)
        {
            const auto& pipeEntry = m_pipelines[size_t(m_currentPipeline - 1)];
            configureVertexAttribs(g, pipeEntry.topology, pipeEntry.vertexFormat, offset);
        }
    }

    void GLDevice::bindIndexBuffer(BufferHandle handle, uint64_t offset)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        if (g->VertexArrayElementBuffer)
        {
            g->VertexArrayElementBuffer(m_vao, entry.glName);
        }
        else
        {
            g->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry.glName);
        }

        m_currentIBO = handle;
        m_currentIBOOffset = offset;
    }

    void GLDevice::bindUniformBuffer(
        uint32_t /*set*/, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        g->BindBufferRange(GL_UNIFORM_BUFFER, binding, entry.glName, (GLintptr)offset, (GLsizeiptr)size);
    }

    void GLDevice::bindShaderStorageBuffer(
        uint32_t /*set*/, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size)
    {
        if (!m_initialized)
        {
            return;
        }
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        g->BindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, entry.glName, (GLintptr)offset, (GLsizeiptr)size);
    }

    void GLDevice::bindTexture(uint32_t /*set*/, uint32_t binding, TextureHandle handle)
    {
        if (handle == NullHandle)
        {
            return;
        }
        auto& entry = m_textures[size_t(handle - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        if (g->BindTextureUnit)
        {
            g->BindTextureUnit(binding, entry.glName);
        }
        else
        {
            g->ActiveTexture(GL_TEXTURE0 + binding);
            g->BindTexture(GL_TEXTURE_2D, entry.glName);
        }
    }

    void GLDevice::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t /*firstInstance*/)
    {
        auto* g = gl();
        uint32_t topo = GL_TRIANGLES;
        if (m_currentPipeline != NullHandle)
        {
            topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
        }

        if (instanceCount > 1)
        {
            g->DrawArraysInstanced(topo, (GLint)firstVertex, (GLsizei)vertexCount, (GLsizei)instanceCount);
        }
        else
        {
            g->DrawArrays(topo, (GLint)firstVertex, (GLsizei)vertexCount);
        }
    }

    void GLDevice::drawIndexed(uint32_t indexCount,
        uint32_t instanceCount,
        uint32_t firstIndex,
        int32_t /*vertexOffset*/,
        uint32_t /*firstInstance*/)
    {
        auto* g = gl();
        uint32_t topo = GL_TRIANGLES;
        if (m_currentPipeline != NullHandle)
        {
            topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
        }

        uintptr_t indices = (uintptr_t)(firstIndex * sizeof(uint32_t)) + m_currentIBOOffset;

        if (instanceCount > 1)
        {
            g->DrawElementsInstanced(
                topo, (GLsizei)indexCount, GL_UNSIGNED_INT, (const void*)indices, (GLsizei)instanceCount);
        }
        else
        {
            g->DrawElements(topo, (GLsizei)indexCount, GL_UNSIGNED_INT, (const void*)indices);
        }
    }

    void GLDevice::drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        if (indirectBuffer == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(indirectBuffer - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        uint32_t topo = GL_TRIANGLES;
        if (m_currentPipeline != NullHandle)
        {
            topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
        }

        g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, entry.glName);

        // macOS OpenGL.framework 仅导出 OpenGL 4.1 core 符号，不导出 ARB 扩展
        // glMultiDrawArraysIndirect，解析指针为 null。跨平台兜底：当 Multi
        // 接口不可用时退化为循环单 DrawArraysIndirect，保证在不支持扩展的平台
        // 上仍能正确绘制。
        if (drawCount == 1)
        {
            g->DrawArraysIndirect(topo, (const void*)(uintptr_t)offset);
        }
        else if (g->MultiDrawArraysIndirect)
        {
            g->MultiDrawArraysIndirect(topo, (const void*)(uintptr_t)offset, (GLsizei)drawCount, (GLsizei)stride);
        }
        else
        {
            for (uint32_t i = 0; i < drawCount; ++i)
            {
                g->DrawArraysIndirect(topo, (const void*)(uintptr_t)(offset + i * stride));
            }
        }
        g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    void GLDevice::drawIndexedIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride)
    {
        if (indirectBuffer == NullHandle)
        {
            return;
        }
        auto& entry = m_buffers[size_t(indirectBuffer - 1)];
        if (!entry.glName)
        {
            return;
        }

        auto* g = gl();
        uint32_t topo = GL_TRIANGLES;
        if (m_currentPipeline != NullHandle)
        {
            topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
        }

        g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, entry.glName);
        if (drawCount == 1)
        {
            g->DrawElementsIndirect(topo, GL_UNSIGNED_INT, (const void*)(uintptr_t)offset);
        }
        else if (g->MultiDrawElementsIndirect)
        {
            g->MultiDrawElementsIndirect(
                topo, GL_UNSIGNED_INT, (const void*)(uintptr_t)offset, (GLsizei)drawCount, (GLsizei)stride);
        }
        else
        {
            for (uint32_t i = 0; i < drawCount; ++i)
            {
                g->DrawElementsIndirect(topo, GL_UNSIGNED_INT, (const void*)(uintptr_t)(offset + i * stride));
            }
        }
        g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
    }

    void GLDevice::dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        if (!m_initialized)
        {
            SY_ERROR("[rhiGl] dispatchCompute called before initialization");
            return;
        }
        auto* g = gl();
        if (g->DispatchCompute)
        {
            g->DispatchCompute(groupsX, groupsY, groupsZ);
        }
        else
        {
            SY_WARN("[rhiGl] DispatchCompute not available (requires OpenGL 4.3+)");
        }
    }

    void GLDevice::memoryBarrier(uint32_t barrierFlags)
    {
        if (!m_initialized)
        {
            SY_ERROR("[rhiGl] memoryBarrier called before initialization");
            return;
        }
        auto* g = gl();
        if (g->MemoryBarrier)
        {
            g->MemoryBarrier(static_cast<GLbitfield>(barrierFlags));
        }
        else
        {
            SY_WARN("[rhiGl] MemoryBarrier not available (requires OpenGL 4.2+)");
        }
    }

    void GLDevice::setViewport(const Viewport& vp)
    {
        auto* g = gl();
        g->Viewport((GLint)vp.x, (GLint)vp.y, (GLsizei)vp.w, (GLsizei)vp.h);
    }

    void GLDevice::setScissor(const Scissor& sc)
    {
        auto* g = gl();
        g->Enable(GL_SCISSOR_TEST);
        g->Scissor(sc.x, sc.y, (GLsizei)sc.w, (GLsizei)sc.h);
    }

    void GLDevice::setLineWidth(float width)
    {
        auto* g = gl();
        // 钳制到 GPU 支持的线宽范围
        // macOS CoreProfile 下 m_maxLineWidth=1.0，所有线宽退化为 1px（OpenGL 规范允许）
        // Windows 下可能支持更宽，但仍需钳制到驱动报告的范围
        float clampedWidth = width;
        if (clampedWidth < m_minLineWidth)
            clampedWidth = m_minLineWidth;
        if (clampedWidth > m_maxLineWidth)
            clampedWidth = m_maxLineWidth;
        g->LineWidth(clampedWidth);
    }

    void GLDevice::setUniformMatrix3(const char* name, const float* data)
    {
        if (!name || !data)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->UniformMatrix3fv(loc, 1, GL_FALSE, data);
        }
    }

    void GLDevice::setUniformMatrix4(const char* name, const float* data)
    {
        if (!name || !data)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->UniformMatrix4fv(loc, 1, GL_FALSE, data);
        }
    }

    void GLDevice::setUniformFloat(const char* name, float value)
    {
        if (!name)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->Uniform1f(loc, value);
        }
    }

    void GLDevice::setUniformInt(const char* name, int32_t value)
    {
        if (!name)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->Uniform1i(loc, value);
        }
    }

    void GLDevice::setUniformVec2(const char* name, const float* data)
    {
        if (!name || !data)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->Uniform2fv(loc, 1, data);
        }
    }

    void GLDevice::setUniformVec3(const char* name, const float* data)
    {
        if (!name || !data)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->Uniform3fv(loc, 1, data);
        }
    }

    void GLDevice::setUniformVec4(const char* name, const float* data)
    {
        if (!name || !data)
        {
            return;
        }
        if (m_currentPipeline == NullHandle)
        {
            return;
        }
        auto& entry = m_pipelines[size_t(m_currentPipeline - 1)];
        if (!entry.program)
        {
            return;
        }

        auto* g = gl();
        auto it = entry.uniformLocations.find(name);
        GLint loc = (it != entry.uniformLocations.end()) ? it->second : g->GetUniformLocation(entry.program, name);
        if (loc >= 0)
        {
            g->Uniform4fv(loc, 1, data);
        }
    }

    void GLDevice::setClearColor(float r, float g, float b, float a)
    {
        m_clearColor[0] = r;
        m_clearColor[1] = g;
        m_clearColor[2] = b;
        m_clearColor[3] = a;
    }

    void GLDevice::clear(uint32_t flags)
    {
        auto* g = gl();
        GLbitfield mask = 0;
        if (flags & GL_COLOR_BUFFER_BIT)
        {
            mask |= GL_COLOR_BUFFER_BIT;
        }
        if (flags & GL_DEPTH_BUFFER_BIT)
        {
            mask |= GL_DEPTH_BUFFER_BIT;
        }
        g->ClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
        g->Clear(mask);
    }

    void GLDevice::enableDepthTest(bool enable)
    {
        auto* g = gl();
        if (enable)
        {
            g->Enable(GL_DEPTH_TEST);
        }
        else
        {
            g->Disable(GL_DEPTH_TEST);
        }
        m_depthTestEnabled = enable;
    }

    void GLDevice::enableBlend(bool enable)
    {
        auto* g = gl();
        if (enable)
        {
            g->Enable(GL_BLEND);
        }
        else
        {
            g->Disable(GL_BLEND);
        }
        m_blendEnabled = enable;
    }

    void GLDevice::resize(uint32_t width, uint32_t height)
    {
        m_width = width;
        m_height = height;
        auto* g = gl();
        g->Viewport(0, 0, (GLsizei)width, (GLsizei)height);
    }

    uint64_t GLDevice::getGPUMemoryUsage() const
    {
        uint64_t total = 0;
        for (const auto& entry : m_buffers)
        {
            total += entry.size;
        }
        for (const auto& entry : m_textures)
        {
            uint64_t bpp = 4;
            switch (entry.format)
            {
            case Format::RGBA8:
                bpp = 4;
                break;
            case Format::RGBA32F:
                bpp = 16;
                break;
            case Format::RG32F:
                bpp = 8;
                break;
            case Format::R32F:
                bpp = 4;
                break;
            case Format::D32F:
                bpp = 4;
                break;
            case Format::D24S8:
                bpp = 4;
                break;
            case Format::R8:
                bpp = 1;
                break;
            }
            total += entry.width * entry.height * bpp * entry.mipLevels;
        }
        return total;
    }

    void* GLDevice::getNativeContext()
    {
        return m_nativeContext;
    }

    // ============================================================================
    // 离屏渲染目标（截图 / 离屏合成）
    // ============================================================================

    namespace
    {
        GLuint depthInternalFormatToGL(Render::DepthFormat fmt)
        {
            switch (fmt)
            {
            case Render::DepthFormat::D32F:
                return GL_DEPTH_COMPONENT32F;
            case Render::DepthFormat::D24S8:
            default:
                return GL_DEPTH24_STENCIL8;
            }
        }
    }  // namespace

    RenderTargetHandle GLDevice::createRenderTarget(const RenderTargetDesc& desc)
    {
        if (desc.width == 0 || desc.height == 0)
        {
            SY_WARNF("GLDevice::createRenderTarget: invalid size %ux%u", desc.width, desc.height);
            return NullRenderTarget;
        }

        auto* g = gl();
        if (!g->GenFramebuffers || !g->BindFramebuffer || !g->FramebufferTexture2D)
        {
            SY_ERROR("GLDevice::createRenderTarget: framebuffer functions unavailable");
            return NullRenderTarget;
        }

        GLRenderTargetEntry e{};
        e.width = desc.width;
        e.height = desc.height;

        // 颜色附件（RGBA8 纹理）
        if (g->CreateTextures)
        {
            g->CreateTextures(GL_TEXTURE_2D, 1, &e.colorTex);
            g->TextureStorage2D(e.colorTex, 1, GL_RGBA8, (GLsizei)desc.width, (GLsizei)desc.height);
            g->TextureParameteri(e.colorTex, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g->TextureParameteri(e.colorTex, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g->TextureParameteri(e.colorTex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->TextureParameteri(e.colorTex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        else
        {
            g->GenTextures(1, &e.colorTex);
            g->BindTexture(GL_TEXTURE_2D, e.colorTex);
            g->TexImage2D(GL_TEXTURE_2D,
                0,
                GL_RGBA8,
                (GLsizei)desc.width,
                (GLsizei)desc.height,
                0,
                GL_RGBA,
                GL_UNSIGNED_BYTE,
                nullptr);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            g->BindTexture(GL_TEXTURE_2D, 0);
        }

        // 深度附件（可选）
        if (desc.depth)
        {
            if (g->GenRenderbuffers && g->BindRenderbuffer && g->RenderbufferStorage && g->FramebufferRenderbuffer)
            {
                g->GenRenderbuffers(1, &e.depthRb);
                g->BindRenderbuffer(GL_RENDERBUFFER, e.depthRb);
                g->RenderbufferStorage(GL_RENDERBUFFER,
                    depthInternalFormatToGL(desc.depthFormat),
                    (GLsizei)desc.width,
                    (GLsizei)desc.height);
                g->BindRenderbuffer(GL_RENDERBUFFER, 0);
            }
            else
            {
                SY_WARNF("GLDevice::createRenderTarget: renderbuffer functions unavailable, depth disabled");
                e.depthRb = 0;
            }
        }

        // 组装 FBO
        g->GenFramebuffers(1, &e.fbo);
        g->BindFramebuffer(GL_FRAMEBUFFER, e.fbo);
        g->FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, e.colorTex, 0);
        if (e.depthRb != 0)
        {
            g->FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, e.depthRb);
        }

        GLenum status = g->CheckFramebufferStatus ? g->CheckFramebufferStatus(GL_FRAMEBUFFER) : GL_FRAMEBUFFER_COMPLETE;
        g->BindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE)
        {
            SY_ERRORF("GLDevice::createRenderTarget: framebuffer incomplete (0x%04X)", static_cast<uint32_t>(status));
            if (e.fbo)
                g->DeleteFramebuffers(1, &e.fbo);
            if (e.colorTex)
                g->DeleteTextures(1, &e.colorTex);
            if (e.depthRb)
                g->DeleteRenderbuffers(1, &e.depthRb);
            return NullRenderTarget;
        }

        // 分配句柄
        RenderTargetHandle h = 0;
        if (!m_renderTargetFreeList.empty())
        {
            h = m_renderTargetFreeList.back();
            m_renderTargetFreeList.pop_back();
            m_renderTargets[size_t(h - 1)] = e;
        }
        else
        {
            m_renderTargets.push_back(e);
            h = static_cast<RenderTargetHandle>(m_renderTargets.size());  // 1-based
        }
        return h;
    }

    void GLDevice::destroyRenderTarget(RenderTargetHandle handle)
    {
        if (handle == NullRenderTarget || handle > m_renderTargets.size())
        {
            return;
        }
        auto& e = m_renderTargets[size_t(handle - 1)];
        auto* g = gl();
        if (e.fbo)
            g->DeleteFramebuffers(1, &e.fbo);
        if (e.colorTex)
            g->DeleteTextures(1, &e.colorTex);
        if (e.depthRb)
            g->DeleteRenderbuffers(1, &e.depthRb);
        e = GLRenderTargetEntry{};
        m_renderTargetFreeList.push_back(static_cast<uint32_t>(handle));
    }

    void GLDevice::bindRenderTarget(RenderTargetHandle handle)
    {
        if (handle == NullRenderTarget || handle > m_renderTargets.size())
        {
            return;
        }
        auto& e = m_renderTargets[size_t(handle - 1)];
        auto* g = gl();

        if (!m_renderTargetBound)
        {
            GLint bound = 0;
            g->GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
            m_savedFramebuffer = static_cast<GLuint>(bound);
            m_savedWidth = static_cast<int32_t>(m_width);
            m_savedHeight = static_cast<int32_t>(m_height);
            m_renderTargetBound = true;
        }

        g->BindFramebuffer(GL_FRAMEBUFFER, e.fbo);
        m_width = e.width;
        m_height = e.height;
        g->Viewport(0, 0, (GLsizei)e.width, (GLsizei)e.height);
    }

    void GLDevice::bindDefaultTarget()
    {
        if (!m_renderTargetBound)
        {
            return;
        }
        auto* g = gl();
        g->BindFramebuffer(GL_FRAMEBUFFER, m_savedFramebuffer);
        m_width = static_cast<uint32_t>(m_savedWidth);
        m_height = static_cast<uint32_t>(m_savedHeight);
        g->Viewport(0, 0, (GLsizei)m_width, (GLsizei)m_height);
        m_renderTargetBound = false;
    }

    void GLDevice::readRenderTarget(RenderTargetHandle handle, void* rgba8, uint32_t rowPitchBytes)
    {
        if (handle == NullRenderTarget || handle > m_renderTargets.size() || !rgba8)
        {
            return;
        }
        auto& e = m_renderTargets[size_t(handle - 1)];
        auto* g = gl();
        if (!g->ReadPixels)
        {
            return;
        }
        g->BindFramebuffer(GL_READ_FRAMEBUFFER, e.fbo);
        // GL 行间距固定为 width*4；若调用方要求更宽行距则逐行拷贝。
        if (rowPitchBytes == e.width * 4 || rowPitchBytes == 0)
        {
            g->ReadPixels(0, 0, (GLsizei)e.width, (GLsizei)e.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba8);
        }
        else
        {
            const uint32_t tight = e.width * 4;
            uint8_t* dst = static_cast<uint8_t*>(rgba8);
            std::vector<uint8_t> row(tight);
            for (uint32_t y = 0; y < e.height; ++y)
            {
                g->ReadPixels(0, (GLsizei)y, (GLsizei)e.width, 1, GL_RGBA, GL_UNSIGNED_BYTE, row.data());
                std::memcpy(dst + y * rowPitchBytes, row.data(), tight);
            }
        }
    }

    uint32_t GLDevice::shaderStageToGL(ShaderStage stage) const
    {
        switch (stage)
        {
        case ShaderStage::Vertex:
            return GL_VERTEX_SHADER;
        case ShaderStage::Fragment:
            return GL_FRAGMENT_SHADER;
        case ShaderStage::Compute:
            return GL_COMPUTE_SHADER;
        }
        return GL_VERTEX_SHADER;
    }

    uint32_t GLDevice::compileShader(uint32_t type, const char* source)
    {
        if (!source || source[0] == '\0')
        {
            return 0;
        }

        auto* g = gl();
        uint32_t shader = g->CreateShader(type);
        g->ShaderSource(shader, 1, &source, nullptr);
        g->CompileShader(shader);

        GLint success = 0;
        g->GetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            GLint logLen = 0;
            g->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
            if (logLen > 0)
            {
                char* log = new char[logLen];
                g->GetShaderInfoLog(shader, logLen, nullptr, log);
                SY_ERRORF("[rhiGl] Shader compile error:\n%s", log);
                delete[] log;
            }
            g->DeleteShader(shader);
            return 0;
        }
        return shader;
    }

    uint32_t GLDevice::compileShaderSPIRV(
        ShaderStage stage, const uint32_t* spirvWords, uint32_t wordCount, const char* entryPoint)
    {
        if (!spirvWords || wordCount == 0)
        {
            return 0;
        }

        auto* g = gl();
        if (!g->ShaderBinary || !g->SpecializeShader)
        {
            SY_ERROR("[rhiGl] SPIR-V not supported by current OpenGL context");
            return 0;
        }

        uint32_t glType = shaderStageToGL(stage);
        uint32_t shader = g->CreateShader(glType);
        if (!shader)
        {
            SY_ERROR("[rhiGl] Failed to create shader object for SPIR-V");
            return 0;
        }

        g->ShaderBinary(1, &shader, GL_SHADER_BINARY_FORMAT_SPIR_V, spirvWords, wordCount * sizeof(uint32_t));

        const char* ep = entryPoint ? entryPoint : "main";
        g->SpecializeShader(shader, ep, 0, nullptr, nullptr);

        GLint success = 0;
        g->GetShaderiv(shader, GL_COMPILE_STATUS, &success);
        if (!success)
        {
            GLint logLen = 0;
            g->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
            if (logLen > 0)
            {
                char* log = new char[logLen];
                g->GetShaderInfoLog(shader, logLen, nullptr, log);
                SY_ERRORF("[rhiGl] SPIR-V specialization error:\n%s", log);
                delete[] log;
            }
            g->DeleteShader(shader);
            return 0;
        }
        return shader;
    }

    uint32_t GLDevice::createComputeProgram(const ShaderModuleDesc* modules, uint32_t count)
    {
        if (!modules || count == 0)
        {
            return 0;
        }

        const ShaderModuleDesc* csModule = nullptr;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (modules[i].stage == ShaderStage::Compute)
            {
                csModule = &modules[i];
                break;
            }
        }
        if (!csModule)
        {
            SY_ERROR("[rhiGl] No compute shader module found");
            return 0;
        }

        uint32_t cs = 0;
        if (csModule->spirvWords && csModule->spirvWordCount > 0)
        {
            cs = compileShaderSPIRV(
                ShaderStage::Compute, csModule->spirvWords, csModule->spirvWordCount, csModule->entryPoint);
        }
        else if (csModule->source)
        {
            cs = compileShader(GL_COMPUTE_SHADER, csModule->source);
        }

        if (!cs)
        {
            return 0;
        }

        auto* g = gl();
        uint32_t prog = g->CreateProgram();
        g->AttachShader(prog, cs);
        g->LinkProgram(prog);
        g->DeleteShader(cs);

        GLint success = 0;
        g->GetProgramiv(prog, GL_LINK_STATUS, &success);
        if (!success)
        {
            GLint logLen = 0;
            g->GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
            if (logLen > 0)
            {
                char* log = new char[logLen];
                g->GetProgramInfoLog(prog, logLen, nullptr, log);
                SY_ERRORF("[rhiGl] Compute program link error:\n%s", log);
                delete[] log;
            }
            g->DeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    uint32_t GLDevice::linkProgram(uint32_t vs, uint32_t fs)
    {
        auto* g = gl();
        uint32_t prog = g->CreateProgram();
        g->AttachShader(prog, vs);
        g->AttachShader(prog, fs);
        g->LinkProgram(prog);

        GLint success = 0;
        g->GetProgramiv(prog, GL_LINK_STATUS, &success);
        if (!success)
        {
            GLint logLen = 0;
            g->GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
            if (logLen > 0)
            {
                char* log = new char[logLen];
                g->GetProgramInfoLog(prog, logLen, nullptr, log);
                SY_ERRORF("[rhiGl] Program link error:\n%s", log);
                delete[] log;
            }
            g->DeleteProgram(prog);
            return 0;
        }
        return prog;
    }

    uint32_t GLDevice::readRegionNonBgPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const float bg[4], int tol)
    {
        auto* g = gl();
        if (!g->ReadPixels || w == 0 || h == 0 || w > 8192 || h > 8192)
        {
            return 0;
        }

        std::vector<unsigned char> pixels(static_cast<size_t>(w) * h * 4);
        g->ReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

        const int br = static_cast<int>(bg[0] * 255.0f);
        const int bgc = static_cast<int>(bg[1] * 255.0f);
        const int bb = static_cast<int>(bg[2] * 255.0f);

        uint32_t nonBg = 0;
        for (size_t i = 0; i < pixels.size(); i += 4)
        {
            const int dr = static_cast<int>(pixels[i]) - br;
            const int dg = static_cast<int>(pixels[i + 1]) - bgc;
            const int db = static_cast<int>(pixels[i + 2]) - bb;
            if (dr * dr + dg * dg + db * db > tol)
            {
                ++nonBg;
            }
        }
        return nonBg;
    }

    int GLDevice::readPixels(
        uint32_t x, uint32_t y, uint32_t width, uint32_t height, void* outPixels, uint32_t* outRowPitch)
    {
        auto* g = gl();
        if (!g->ReadPixels || width == 0 || height == 0 || width > 8192 || height > 8192 || !outPixels)
        {
            return 0;
        }

        // 对齐到 4 字节边界（GL_PACK_ALIGNMENT 默认为 4）
        uint32_t rowPitch = ((width * 4 + 3) / 4) * 4;
        if (outRowPitch)
        {
            *outRowPitch = rowPitch;
        }

        g->ReadPixels((GLint)x, (GLint)y, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, outPixels);
        return 1;
    }

    uint32_t GLDevice::topologyToGL(PrimitiveTopology topo) const
    {
        switch (topo)
        {
        case PrimitiveTopology::PointList:
            return GL_POINTS;
        case PrimitiveTopology::LineList:
            return GL_LINES;
        case PrimitiveTopology::LineStrip:
            return GL_LINE_STRIP;
        case PrimitiveTopology::LineLoop:
            return GL_LINE_LOOP;
        case PrimitiveTopology::TriangleList:
            return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip:
            return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::TriangleFan:
            return GL_TRIANGLE_FAN;
        }
        return GL_TRIANGLES;
    }

    uint32_t GLDevice::formatToGLInternal(Format fmt) const
    {
        switch (fmt)
        {
        case Format::RGBA8:
            return GL_RGBA8;
        case Format::RGBA32F:
            return 0x8814;
        case Format::RG32F:
            return 0x8230;
        case Format::R32F:
            return 0x822E;
        case Format::D32F:
            return 0x8CAC;
        case Format::D24S8:
            return 0x88F0;
        case Format::R8:
            return 0x8229;
        }
        return GL_RGBA8;
    }

    uint32_t GLDevice::formatToGLFormat(Format fmt) const
    {
        switch (fmt)
        {
        case Format::RGBA8:
        case Format::RGBA32F:
            return GL_RGBA;
        case Format::RG32F:
            return 0x8227;
        case Format::R32F:
            return 0x1903;
        case Format::D32F:
            return 0x1902;
        case Format::D24S8:
            return 0x84F9;
        case Format::R8:
            return 0x1903;
        }
        return GL_RGBA;
    }

    uint32_t GLDevice::formatToGLType(Format fmt) const
    {
        switch (fmt)
        {
        case Format::RGBA8:
            return GL_UNSIGNED_BYTE;
        case Format::RGBA32F:
        case Format::RG32F:
        case Format::R32F:
        case Format::D32F:
            return GL_FLOAT;
        case Format::D24S8:
            return 0x84FA;
        case Format::R8:
            return GL_UNSIGNED_BYTE;
        }
        return GL_UNSIGNED_BYTE;
    }

    uint32_t GLDevice::getFormatBytesPerPixel(Format fmt) const
    {
        switch (fmt)
        {
        case Format::RGBA8:
            return 4;
        case Format::RGBA32F:
            return 16;
        case Format::RG32F:
            return 8;
        case Format::R32F:
            return 4;
        case Format::D32F:
            return 4;
        case Format::D24S8:
            return 4;
        case Format::R8:
            return 1;
        }
        return 4;
    }

    uint32_t GLDevice::vertexFormatStride(VertexFormat fmt) const
    {
        switch (fmt)
        {
        case VertexFormat::P3C3:
            return 6 * sizeof(float);  // pos(3) + col(3) = 24
        case VertexFormat::P3C4:
            return 7 * sizeof(float);  // pos(3) + col(4) = 28
        case VertexFormat::P3N3:
            return 6 * sizeof(float);  // pos(3) + nor(3) = 24
        case VertexFormat::P3T2:
            return 5 * sizeof(float);  // pos(3) + uv(2) = 20
        case VertexFormat::P3T2C4:
            return 9 * sizeof(float);  // pos(3) + uv(2) + col(4) = 36
        case VertexFormat::P2T2C4:
            return 8 * sizeof(float);  // pos(2) + uv(2) + col(4) = 32
        }
        return 0;
    }

    void GLDevice::configureVertexAttribs(GLFuncs* g, PrimitiveTopology /*topo*/, VertexFormat fmt, uint64_t baseOffset)
    {
        if (!g->BindVertexArray)
        {
            g->BindVertexArray(m_vao);
        }

        switch (fmt)
        {
        case VertexFormat::P3C3:
        {
            if (g->VertexArrayAttribFormat)
            {
                g->VertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                g->VertexArrayAttribFormat(m_vao, 1, 3, GL_FLOAT, GL_FALSE, 12);
                g->EnableVertexArrayAttrib(m_vao, 0);
                g->EnableVertexArrayAttrib(m_vao, 1);
                g->VertexArrayAttribBinding(m_vao, 0, 0);
                g->VertexArrayAttribBinding(m_vao, 1, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)(uintptr_t)(baseOffset + 12));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        case VertexFormat::P3C4:
        {
            if (g->VertexArrayAttribFormat)
            {
                g->VertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                g->VertexArrayAttribFormat(m_vao, 1, 4, GL_FLOAT, GL_FALSE, 12);
                g->EnableVertexArrayAttrib(m_vao, 0);
                g->EnableVertexArrayAttrib(m_vao, 1);
                g->VertexArrayAttribBinding(m_vao, 0, 0);
                g->VertexArrayAttribBinding(m_vao, 1, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 28, (void*)(uintptr_t)(baseOffset + 12));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        case VertexFormat::P3N3:
        {
            if (g->VertexArrayAttribFormat)
            {
                g->VertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                g->VertexArrayAttribFormat(m_vao, 1, 3, GL_FLOAT, GL_FALSE, 12);
                g->EnableVertexArrayAttrib(m_vao, 0);
                g->EnableVertexArrayAttrib(m_vao, 1);
                g->VertexArrayAttribBinding(m_vao, 0, 0);
                g->VertexArrayAttribBinding(m_vao, 1, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void*)(uintptr_t)(baseOffset + 12));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        case VertexFormat::P3T2:
        {
            if (g->VertexArrayAttribFormat)
            {
                g->VertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                g->VertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 12);
                g->EnableVertexArrayAttrib(m_vao, 0);
                g->EnableVertexArrayAttrib(m_vao, 1);
                g->VertexArrayAttribBinding(m_vao, 0, 0);
                g->VertexArrayAttribBinding(m_vao, 1, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 20, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 20, (void*)(uintptr_t)(baseOffset + 12));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        case VertexFormat::P3T2C4:
        {
            if (g->VertexArrayAttribFormat)
            {
                g->VertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, 0);
                g->VertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, 12);
                g->VertexArrayAttribFormat(m_vao, 2, 4, GL_FLOAT, GL_FALSE, 20);
                g->EnableVertexArrayAttrib(m_vao, 0);
                g->EnableVertexArrayAttrib(m_vao, 1);
                g->EnableVertexArrayAttrib(m_vao, 2);
                g->VertexArrayAttribBinding(m_vao, 0, 0);
                g->VertexArrayAttribBinding(m_vao, 1, 0);
                g->VertexArrayAttribBinding(m_vao, 2, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 36, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 36, (void*)(uintptr_t)(baseOffset + 12));
                g->VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 36, (void*)(uintptr_t)(baseOffset + 20));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->EnableVertexAttribArray(2);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        case VertexFormat::P2T2C4:
        {
            if (g->BindVertexBuffer)
            {
                g->BindVertexBuffer(0, m_buffers[size_t(m_currentVBOs[0] - 1)].glName, (GLintptr)baseOffset, 32);
                g->VertexAttribFormat(0, 2, GL_FLOAT, GL_FALSE, 0);
                g->VertexAttribFormat(1, 2, GL_FLOAT, GL_FALSE, 8);
                g->VertexAttribFormat(2, 4, GL_FLOAT, GL_FALSE, 16);
                g->VertexAttribBinding(0, 0);
                g->VertexAttribBinding(1, 0);
                g->VertexAttribBinding(2, 0);
            }
            else
            {
                g->BindBuffer(GL_ARRAY_BUFFER, m_buffers[size_t(m_currentVBOs[0] - 1)].glName);
                g->VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 32, (void*)(uintptr_t)(baseOffset + 0));
                g->VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)(uintptr_t)(baseOffset + 8));
                g->VertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 32, (void*)(uintptr_t)(baseOffset + 16));
                g->EnableVertexAttribArray(0);
                g->EnableVertexAttribArray(1);
                g->EnableVertexAttribArray(2);
                g->BindBuffer(GL_ARRAY_BUFFER, 0);
            }
            break;
        }
        }
    }

    IDevice* createGLDevice()
    {
        return new GLDevice();
    }
}  // namespace Render::RHI