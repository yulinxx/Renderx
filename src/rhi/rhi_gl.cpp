#include "rhi_gl.h"
#include "platform/gl_loader.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
  #include <GL/gl.h>
#elif defined(__linux__)
  #include <GL/glx.h>
#endif

namespace render::rhi {

GLDevice::~GLDevice() {
    if (m_initialized) shutdown();
}

bool GLDevice::initialize(void* nativeWindow, uint32_t width, uint32_t height) {
    m_nativeContext = nativeWindow;
    m_width = width;
    m_height = height;

#ifdef _WIN32
    auto getProcAddr = [](const char* name) -> void* {
        return (void*)wglGetProcAddress(name);
    };
#elif defined(__linux__)
    auto getProcAddr = [](const char* name) -> void* {
        return (void*)glXGetProcAddress((const GLubyte*)name);
    };
#elif defined(__APPLE__)
    auto getProcAddr = [](const char* name) -> void* {
        return nullptr;
    };
#endif

    if (!gl_loader_init(nullptr)) {
        std::fprintf(stderr, "[RHI_GL] gl_loader_init failed\n");
        return false;
    }

    auto* g = gl();

    if (g->CreateVertexArrays) {
        g->CreateVertexArrays(1, &m_vao);
    } else {
        g->GenVertexArrays(1, &m_vao);
    }
    if (g->BindVertexArray) {
        g->BindVertexArray(m_vao);
    }

    g->Enable(GL_MULTISAMPLE);
    g->Enable(GL_LINE_SMOOTH);

    m_initialized = true;
    return true;
}

void GLDevice::shutdown() {
    if (!m_initialized) return;

    auto* g = gl();

    for (auto& entry : m_buffers) {
        if (entry.glName) {
            g->DeleteBuffers(1, &entry.glName);
            entry.glName = 0;
        }
    }
    m_buffers.clear();
    m_bufferFreeList.clear();

    for (auto& entry : m_textures) {
        if (entry.glName) {
            g->DeleteTextures(1, &entry.glName);
            entry.glName = 0;
        }
    }
    m_textures.clear();
    m_textureFreeList.clear();

    for (auto& entry : m_pipelines) {
        if (entry.program) {
            g->DeleteProgram(entry.program);
            entry.program = 0;
        }
    }
    m_pipelines.clear();
    m_pipelineFreeList.clear();

    if (m_vao) {
        g->DeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    m_initialized = false;
}

BufferHandle GLDevice::allocBufferHandle() {
    if (!m_bufferFreeList.empty()) {
        uint32_t idx = m_bufferFreeList.back();
        m_bufferFreeList.pop_back();
        return BufferHandle(idx + 1);
    }
    m_buffers.emplace_back();
    return BufferHandle(m_buffers.size());
}

TextureHandle GLDevice::allocTextureHandle() {
    if (!m_textureFreeList.empty()) {
        uint32_t idx = m_textureFreeList.back();
        m_textureFreeList.pop_back();
        return TextureHandle(idx + 1);
    }
    m_textures.emplace_back();
    return TextureHandle(m_textures.size());
}

PipelineHandle GLDevice::allocPipelineHandle() {
    if (!m_pipelineFreeList.empty()) {
        uint32_t idx = m_pipelineFreeList.back();
        m_pipelineFreeList.pop_back();
        return PipelineHandle(idx + 1);
    }
    m_pipelines.emplace_back();
    return PipelineHandle(m_pipelines.size());
}

BufferHandle GLDevice::createBuffer(const BufferDesc& desc) {
    auto* g = gl();
    BufferHandle handle = allocBufferHandle();
    auto& entry = m_buffers[size_t(handle - 1)];

    entry.size = desc.size;
    entry.usage = desc.usage;
    entry.memory = desc.memory;
    entry.mappedPtr = nullptr;

    if (g->CreateBuffers) {
        g->CreateBuffers(1, &entry.glName);
        g->NamedBufferData(entry.glName, (GLsizeiptr)desc.size, nullptr, GL_DYNAMIC_DRAW);
    } else {
        g->GenBuffers(1, &entry.glName);
        g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
        g->BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)desc.size, nullptr, GL_DYNAMIC_DRAW);
        g->BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    return handle;
}

void GLDevice::destroyBuffer(BufferHandle handle) {
    if (handle == NullHandle) return;
    auto idx = size_t(handle - 1);
    if (idx >= m_buffers.size()) return;

    auto* g = gl();
    auto& entry = m_buffers[idx];
    if (entry.glName) {
        g->DeleteBuffers(1, &entry.glName);
    }
    entry = GLBufferEntry{};
    m_bufferFreeList.push_back(uint32_t(idx));
}

TextureHandle GLDevice::createTexture(const TextureDesc& desc) {
    auto* g = gl();
    TextureHandle handle = allocTextureHandle();
    auto& entry = m_textures[size_t(handle - 1)];

    entry.format = desc.format;
    entry.width = desc.width;
    entry.height = desc.height;
    entry.mipLevels = desc.mipLevels;

    if (g->CreateTextures) {
        g->CreateTextures(GL_TEXTURE_2D, 1, &entry.glName);
        g->TextureStorage2D(entry.glName, desc.mipLevels,
                            formatToGLInternal(desc.format),
                            desc.width, desc.height);
        g->TextureParameteri(entry.glName, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g->TextureParameteri(entry.glName, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g->TextureParameteri(entry.glName, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g->TextureParameteri(entry.glName, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        g->GenTextures(1, &entry.glName);
        g->BindTexture(GL_TEXTURE_2D, entry.glName);
        g->TexImage2D(GL_TEXTURE_2D, 0, formatToGLInternal(desc.format),
                      desc.width, desc.height, 0,
                      formatToGLFormat(desc.format), formatToGLType(desc.format),
                      nullptr);
        g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        g->TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g->BindTexture(GL_TEXTURE_2D, 0);
    }

    return handle;
}

void GLDevice::destroyTexture(TextureHandle handle) {
    if (handle == NullHandle) return;
    auto idx = size_t(handle - 1);
    if (idx >= m_textures.size()) return;

    auto* g = gl();
    auto& entry = m_textures[idx];
    if (entry.glName) {
        g->DeleteTextures(1, &entry.glName);
    }
    entry = GLTextureEntry{};
    m_textureFreeList.push_back(uint32_t(idx));
}

PipelineHandle GLDevice::createPipeline(const PipelineDesc& desc) {
    auto* g = gl();
    PipelineHandle handle = allocPipelineHandle();
    auto& entry = m_pipelines[size_t(handle - 1)];

    uint32_t vs = compileShader(GL_VERTEX_SHADER, desc.vertexShader);
    uint32_t fs = compileShader(GL_FRAGMENT_SHADER, desc.fragmentShader);
    if (!vs || !fs) {
        if (vs) g->DeleteShader(vs);
        if (fs) g->DeleteShader(fs);
        entry = GLPipelineEntry{};
        m_pipelineFreeList.push_back(uint32_t(handle - 1));
        return NullHandle;
    }

    uint32_t prog = linkProgram(vs, fs);
    g->DeleteShader(vs);
    g->DeleteShader(fs);

    if (!prog) {
        entry = GLPipelineEntry{};
        m_pipelineFreeList.push_back(uint32_t(handle - 1));
        return NullHandle;
    }

    entry.program = prog;
    entry.topology = desc.topology;
    entry.depthTest = desc.depthTest;
    entry.depthWrite = desc.depthWrite;
    entry.blendEnable = desc.blendEnable;
    entry.srcBlend = desc.srcBlend;
    entry.dstBlend = desc.dstBlend;
    entry.depthFunc = desc.depthFunc;

    return handle;
}

void GLDevice::destroyPipeline(PipelineHandle handle) {
    if (handle == NullHandle) return;
    auto idx = size_t(handle - 1);
    if (idx >= m_pipelines.size()) return;

    auto* g = gl();
    auto& entry = m_pipelines[idx];
    if (entry.program) {
        g->DeleteProgram(entry.program);
    }
    entry = GLPipelineEntry{};
    m_pipelineFreeList.push_back(uint32_t(idx));
}

void GLDevice::uploadBuffer(BufferHandle handle, uint64_t offset, uint64_t size, const void* data) {
    if (handle == NullHandle || !data) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    if (g->NamedBufferSubData) {
        g->NamedBufferSubData(entry.glName, (GLintptr)offset, (GLsizeiptr)size, data);
    } else {
        GLenum target = GL_ARRAY_BUFFER;
        g->BindBuffer(target, entry.glName);
        g->BufferSubData(target, (GLintptr)offset, (GLsizeiptr)size, data);
        g->BindBuffer(target, 0);
    }
}

void GLDevice::uploadTexture(TextureHandle handle, uint32_t mip, const void* data, uint32_t rowPitch) {
    if (handle == NullHandle || !data) return;
    auto& entry = m_textures[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    uint32_t fmt = formatToGLFormat(entry.format);
    uint32_t type = formatToGLType(entry.format);

    if (g->TextureSubImage2D) {
        g->TextureSubImage2D(entry.glName, (GLint)mip, 0, 0,
                             entry.width, entry.height, fmt, type, data);
    } else {
        g->BindTexture(GL_TEXTURE_2D, entry.glName);
        g->TexSubImage2D(GL_TEXTURE_2D, (GLint)mip, 0, 0,
                         entry.width, entry.height, fmt, type, data);
        g->BindTexture(GL_TEXTURE_2D, 0);
    }
}

void* GLDevice::mapBuffer(BufferHandle handle, uint64_t offset, uint64_t size, uint32_t mapFlags) {
    if (handle == NullHandle) return nullptr;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName) return nullptr;

    auto* g = gl();
    GLbitfield glFlags = 0;
    if (mapFlags & GL_MAP_READ_BIT)   glFlags |= GL_MAP_READ_BIT;
    if (mapFlags & GL_MAP_WRITE_BIT)  glFlags |= GL_MAP_WRITE_BIT;
    if (mapFlags & GL_MAP_INVALIDATE_RANGE_BIT)   glFlags |= GL_MAP_INVALIDATE_RANGE_BIT;
    if (mapFlags & GL_MAP_INVALIDATE_BUFFER_BIT)  glFlags |= GL_MAP_INVALIDATE_BUFFER_BIT;
    if (mapFlags & GL_MAP_FLUSH_EXPLICIT_BIT)     glFlags |= GL_MAP_FLUSH_EXPLICIT_BIT;
    if (mapFlags & GL_MAP_UNSYNCHRONIZED_BIT)     glFlags |= GL_MAP_UNSYNCHRONIZED_BIT;
    if (mapFlags & GL_MAP_PERSISTENT_BIT)         glFlags |= GL_MAP_PERSISTENT_BIT;
    if (mapFlags & GL_MAP_COHERENT_BIT)           glFlags |= GL_MAP_COHERENT_BIT;

    void* ptr = nullptr;
    if (g->MapNamedBufferRange) {
        ptr = g->MapNamedBufferRange(entry.glName, (GLintptr)offset, (GLsizeiptr)size, glFlags);
    } else {
        g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
        ptr = g->MapBufferRange(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size, glFlags);
        g->BindBuffer(GL_ARRAY_BUFFER, 0);
    }

    entry.mappedPtr = ptr;
    return ptr;
}

void GLDevice::unmapBuffer(BufferHandle handle) {
    if (handle == NullHandle) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName || !entry.mappedPtr) return;

    auto* g = gl();
    if (g->UnmapNamedBuffer) {
        g->UnmapNamedBuffer(entry.glName);
    } else {
        g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
        g->UnmapBuffer(GL_ARRAY_BUFFER);
        g->BindBuffer(GL_ARRAY_BUFFER, 0);
    }
    entry.mappedPtr = nullptr;
}

void GLDevice::flushMappedRange(BufferHandle handle, uint64_t offset, uint64_t size) {
    if (handle == NullHandle) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName || !entry.mappedPtr) return;

    auto* g = gl();
    if (g->FlushMappedNamedBufferRange) {
        g->FlushMappedNamedBufferRange(entry.glName, (GLintptr)offset, (GLsizeiptr)size);
    } else {
        g->BindBuffer(GL_ARRAY_BUFFER, entry.glName);
        g->FlushMappedBufferRange(GL_ARRAY_BUFFER, (GLintptr)offset, (GLsizeiptr)size);
        g->BindBuffer(GL_ARRAY_BUFFER, 0);
    }
}

void GLDevice::beginFrame() {
    auto* g = gl();
    g->BindVertexArray(m_vao);
    g->Viewport(0, 0, (GLsizei)m_width, (GLsizei)m_height);
    g->ClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    g->Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_currentPipeline = NullHandle;
    for (int i = 0; i < 4; i++) {
        m_currentVBOs[i] = NullHandle;
        m_currentVBOOffsets[i] = 0;
    }
    m_currentIBO = NullHandle;
    m_currentIBOOffset = 0;
    m_depthTestEnabled = false;
    m_blendEnabled = false;
}

void GLDevice::endFrame() {
    auto* g = gl();
    g->Flush();
}

void GLDevice::present() {
}

void GLDevice::bindPipeline(PipelineHandle handle) {
    if (handle == NullHandle) return;
    auto& entry = m_pipelines[size_t(handle - 1)];
    if (!entry.program) return;

    auto* g = gl();
    g->UseProgram(entry.program);
    m_currentPipeline = handle;

    if (entry.depthTest) {
        g->Enable(GL_DEPTH_TEST);
        m_depthTestEnabled = true;
        uint32_t depthFuncGL = GL_LESS;
        switch (entry.depthFunc) {
            case CompareFunc::Never:     depthFuncGL = 0x0200; break;
            case CompareFunc::Less:      depthFuncGL = GL_LESS; break;
            case CompareFunc::Equal:     depthFuncGL = 0x0202; break;
            case CompareFunc::LessEqual: depthFuncGL = GL_LEQUAL; break;
            case CompareFunc::Greater:   depthFuncGL = 0x0204; break;
            case CompareFunc::Always:    depthFuncGL = 0x0207; break;
        }
        g->DepthFunc(depthFuncGL);
    } else {
        g->Disable(GL_DEPTH_TEST);
        m_depthTestEnabled = false;
    }

    g->DepthMask(entry.depthWrite ? GL_TRUE : GL_FALSE);

    if (entry.blendEnable) {
        g->Enable(GL_BLEND);
        m_blendEnabled = true;
        auto toGLBlend = [](BlendFactor f) -> uint32_t {
            switch (f) {
                case BlendFactor::Zero:           return 0x0000;
                case BlendFactor::One:            return 0x0001;
                case BlendFactor::SrcAlpha:       return GL_SRC_ALPHA;
                case BlendFactor::OneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
            }
            return 0x0000;
        };
        g->BlendFunc(toGLBlend(entry.srcBlend), toGLBlend(entry.dstBlend));
    } else {
        g->Disable(GL_BLEND);
        m_blendEnabled = false;
    }
}

void GLDevice::bindVertexBuffer(uint32_t slot, BufferHandle handle, uint64_t offset) {
    if (handle == NullHandle || slot >= 4) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    if (g->VertexArrayVertexBuffer) {
        g->VertexArrayVertexBuffer(m_vao, slot, entry.glName, (GLintptr)offset, 0);
    }

    m_currentVBOs[slot] = handle;
    m_currentVBOOffsets[slot] = offset;
}

void GLDevice::bindIndexBuffer(BufferHandle handle, uint64_t offset) {
    if (handle == NullHandle) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    if (g->VertexArrayElementBuffer) {
        g->VertexArrayElementBuffer(m_vao, entry.glName);
    } else {
        g->BindBuffer(GL_ELEMENT_ARRAY_BUFFER, entry.glName);
    }

    m_currentIBO = handle;
    m_currentIBOOffset = offset;
}

void GLDevice::bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle handle, uint64_t offset, uint64_t size) {
    if (handle == NullHandle) return;
    auto& entry = m_buffers[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    g->BindBufferRange(GL_UNIFORM_BUFFER, binding, entry.glName, (GLintptr)offset, (GLsizeiptr)size);
}

void GLDevice::bindTexture(uint32_t set, uint32_t binding, TextureHandle handle) {
    if (handle == NullHandle) return;
    auto& entry = m_textures[size_t(handle - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    if (g->BindTextureUnit) {
        g->BindTextureUnit(binding, entry.glName);
    } else {
        g->ActiveTexture(GL_TEXTURE0 + binding);
        g->BindTexture(GL_TEXTURE_2D, entry.glName);
    }
}

void GLDevice::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    auto* g = gl();
    uint32_t topo = GL_TRIANGLES;
    if (m_currentPipeline != NullHandle) {
        topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
    }

    if (instanceCount > 1) {
        g->DrawArraysInstanced(topo, (GLint)firstVertex, (GLsizei)vertexCount, (GLsizei)instanceCount);
    } else {
        g->DrawArrays(topo, (GLint)firstVertex, (GLsizei)vertexCount);
    }
}

void GLDevice::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    auto* g = gl();
    uint32_t topo = GL_TRIANGLES;
    if (m_currentPipeline != NullHandle) {
        topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
    }

    uintptr_t indices = (uintptr_t)(firstIndex * sizeof(uint32_t)) + m_currentIBOOffset;

    if (instanceCount > 1) {
        g->DrawElementsInstanced(topo, (GLsizei)indexCount, GL_UNSIGNED_INT,
                                 (const void*)indices, (GLsizei)instanceCount);
    } else {
        g->DrawElements(topo, (GLsizei)indexCount, GL_UNSIGNED_INT, (const void*)indices);
    }
}

void GLDevice::drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) {
    if (indirectBuffer == NullHandle) return;
    auto& entry = m_buffers[size_t(indirectBuffer - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    uint32_t topo = GL_TRIANGLES;
    if (m_currentPipeline != NullHandle) {
        topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
    }

    g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, entry.glName);
    if (drawCount == 1) {
        g->DrawArraysIndirect(topo, (const void*)(uintptr_t)offset);
    } else {
        g->MultiDrawArraysIndirect(topo, (const void*)(uintptr_t)offset, (GLsizei)drawCount, (GLsizei)stride);
    }
    g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void GLDevice::drawIndexedIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) {
    if (indirectBuffer == NullHandle) return;
    auto& entry = m_buffers[size_t(indirectBuffer - 1)];
    if (!entry.glName) return;

    auto* g = gl();
    uint32_t topo = GL_TRIANGLES;
    if (m_currentPipeline != NullHandle) {
        topo = topologyToGL(m_pipelines[size_t(m_currentPipeline - 1)].topology);
    }

    g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, entry.glName);
    if (drawCount == 1) {
        g->DrawElementsIndirect(topo, GL_UNSIGNED_INT, (const void*)(uintptr_t)offset);
    } else {
        g->MultiDrawElementsIndirect(topo, GL_UNSIGNED_INT, (const void*)(uintptr_t)offset, (GLsizei)drawCount, (GLsizei)stride);
    }
    g->BindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);
}

void GLDevice::setViewport(const Viewport& vp) {
    auto* g = gl();
    g->Viewport((GLint)vp.x, (GLint)vp.y, (GLsizei)vp.w, (GLsizei)vp.h);
}

void GLDevice::setScissor(const Scissor& sc) {
    auto* g = gl();
    g->Enable(GL_SCISSOR_TEST);
    g->Scissor(sc.x, sc.y, (GLsizei)sc.w, (GLsizei)sc.h);
}

void GLDevice::setLineWidth(float width) {
    auto* g = gl();
    g->LineWidth(width);
}

void GLDevice::setClearColor(float r, float g, float b, float a) {
    m_clearColor[0] = r;
    m_clearColor[1] = g;
    m_clearColor[2] = b;
    m_clearColor[3] = a;
}

void GLDevice::clear(uint32_t flags) {
    auto* g = gl();
    GLbitfield mask = 0;
    if (flags & 0x01) mask |= GL_COLOR_BUFFER_BIT;
    if (flags & 0x02) mask |= GL_DEPTH_BUFFER_BIT;
    g->ClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    g->Clear(mask);
}

void GLDevice::enableDepthTest(bool enable) {
    auto* g = gl();
    if (enable) {
        g->Enable(GL_DEPTH_TEST);
    } else {
        g->Disable(GL_DEPTH_TEST);
    }
    m_depthTestEnabled = enable;
}

void GLDevice::enableBlend(bool enable) {
    auto* g = gl();
    if (enable) {
        g->Enable(GL_BLEND);
    } else {
        g->Disable(GL_BLEND);
    }
    m_blendEnabled = enable;
}

void GLDevice::resize(uint32_t width, uint32_t height) {
    m_width = width;
    m_height = height;
    auto* g = gl();
    g->Viewport(0, 0, (GLsizei)width, (GLsizei)height);
}

uint64_t GLDevice::getGPUMemoryUsage() const {
    uint64_t total = 0;
    for (const auto& entry : m_buffers) {
        total += entry.size;
    }
    for (const auto& entry : m_textures) {
        uint64_t bpp = 4;
        switch (entry.format) {
            case Format::RGBA8:   bpp = 4; break;
            case Format::RGBA32F: bpp = 16; break;
            case Format::RG32F:   bpp = 8; break;
            case Format::R32F:    bpp = 4; break;
            case Format::D32F:    bpp = 4; break;
            case Format::D24S8:   bpp = 4; break;
            case Format::R8:      bpp = 1; break;
        }
        total += entry.width * entry.height * bpp * entry.mipLevels;
    }
    return total;
}

void* GLDevice::getNativeContext() {
    return m_nativeContext;
}

uint32_t GLDevice::compileShader(uint32_t type, const char* source) {
    if (!source) return 0;

    auto* g = gl();
    uint32_t shader = g->CreateShader(type);
    g->ShaderSource(shader, 1, &source, nullptr);
    g->CompileShader(shader);

    GLint success = 0;
    g->GetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        GLint logLen = 0;
        g->GetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            g->GetShaderInfoLog(shader, logLen, nullptr, log);
            std::fprintf(stderr, "[RHI_GL] Shader compile error:\n%s\n", log);
            delete[] log;
        }
        g->DeleteShader(shader);
        return 0;
    }
    return shader;
}

uint32_t GLDevice::linkProgram(uint32_t vs, uint32_t fs) {
    auto* g = gl();
    uint32_t prog = g->CreateProgram();
    g->AttachShader(prog, vs);
    g->AttachShader(prog, fs);
    g->LinkProgram(prog);

    GLint success = 0;
    g->GetProgramiv(prog, GL_LINK_STATUS, &success);
    if (!success) {
        GLint logLen = 0;
        g->GetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
        if (logLen > 0) {
            char* log = new char[logLen];
            g->GetProgramInfoLog(prog, logLen, nullptr, log);
            std::fprintf(stderr, "[RHI_GL] Program link error:\n%s\n", log);
            delete[] log;
        }
        g->DeleteProgram(prog);
        return 0;
    }
    return prog;
}

uint32_t GLDevice::topologyToGL(PrimitiveTopology topo) const {
    switch (topo) {
        case PrimitiveTopology::PointList:     return GL_POINTS;
        case PrimitiveTopology::LineList:      return GL_LINES;
        case PrimitiveTopology::LineStrip:     return GL_LINE_STRIP;
        case PrimitiveTopology::LineLoop:      return GL_LINE_LOOP;
        case PrimitiveTopology::TriangleList:  return GL_TRIANGLES;
        case PrimitiveTopology::TriangleStrip: return GL_TRIANGLE_STRIP;
        case PrimitiveTopology::TriangleFan:   return GL_TRIANGLE_FAN;
    }
    return GL_TRIANGLES;
}

uint32_t GLDevice::formatToGLInternal(Format fmt) const {
    switch (fmt) {
        case Format::RGBA8:   return GL_RGBA8;
        case Format::RGBA32F: return 0x8814;
        case Format::RG32F:   return 0x8230;
        case Format::R32F:    return 0x822E;
        case Format::D32F:    return 0x8CAC;
        case Format::D24S8:   return 0x88F0;
        case Format::R8:      return 0x8229;
    }
    return GL_RGBA8;
}

uint32_t GLDevice::formatToGLFormat(Format fmt) const {
    switch (fmt) {
        case Format::RGBA8:
        case Format::RGBA32F: return GL_RGBA;
        case Format::RG32F:   return 0x8227;
        case Format::R32F:    return 0x1903;
        case Format::D32F:    return 0x1902;
        case Format::D24S8:   return 0x84F9;
        case Format::R8:      return 0x1903;
    }
    return GL_RGBA;
}

uint32_t GLDevice::formatToGLType(Format fmt) const {
    switch (fmt) {
        case Format::RGBA8:   return GL_UNSIGNED_BYTE;
        case Format::RGBA32F:
        case Format::RG32F:
        case Format::R32F:
        case Format::D32F:    return GL_FLOAT;
        case Format::D24S8:   return 0x84FA;
        case Format::R8:      return GL_UNSIGNED_BYTE;
    }
    return GL_UNSIGNED_BYTE;
}

IDevice* createGLDevice() {
    return new GLDevice();
}

}
