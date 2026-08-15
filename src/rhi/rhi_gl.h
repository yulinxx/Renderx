/**
 * @file rhi_gl.h
 * @brief OpenGL 渲染设备实现
 *
 * GLDevice 是 IDevice 接口的 OpenGL 实现，提供了基于 OpenGL 4.6 的图形渲染能力。
 * 主要功能包括：
 * - 缓冲区管理（顶点缓冲、索引缓冲、间接命令缓冲）
 * - 纹理管理（2D纹理上传和绑定）
 * - 图形管线管理（着色器编译、程序链接、管线状态设置）
 * - 绘制命令执行（立即绘制、间接绘制、实例化绘制）
 * - 渲染状态管理（深度测试、混合、清除颜色等）
 *
 * 使用 gl_loader 动态加载 OpenGL 函数指针，确保跨平台兼容性。
 */
#pragma once
#include "rhi_device.h"
#include "../platform/gl_loader.h"

#include <vector>
#include <unordered_map>
#include <string>

namespace render::rhi
{

    struct GLBufferEntry
    {
        uint32_t glName = 0;
        uint64_t size = 0;
        BufferUsage usage = BufferUsage::Vertex;
        MemoryType memory = MemoryType::GPU_Only;
        void* mappedPtr = nullptr;
    };

    struct GLPipelineEntry
    {
        uint32_t program = 0;
        PrimitiveTopology topology = PrimitiveTopology::TriangleList;
        VertexFormat vertexFormat = VertexFormat::P3C3;
        bool depthTest = false;
        bool depthWrite = false;
        bool blendEnable = false;
        BlendFactor srcBlend = BlendFactor::One;
        BlendFactor dstBlend = BlendFactor::Zero;
        CompareFunc depthFunc = CompareFunc::Less;

        std::unordered_map<std::string, int32_t> uniformLocations;
    };

    struct GLTextureEntry
    {
        uint32_t glName = 0;
        Format format = Format::RGBA8;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t mipLevels = 1;
    };

    class GLDevice : public IDevice
    {
    public:
        GLDevice() = default;
        ~GLDevice() override;

        bool initialize(void* nativeWindow, uint32_t width, uint32_t height) override;
        void shutdown() override;

        BufferHandle createBuffer(const BufferDesc&) override;
        void destroyBuffer(BufferHandle) override;
        TextureHandle createTexture(const TextureDesc&) override;
        void destroyTexture(TextureHandle) override;
        PipelineHandle createPipeline(const PipelineDesc&) override;
        void destroyPipeline(PipelineHandle) override;

        void uploadBuffer(BufferHandle, uint64_t offset, uint64_t size, const void* data) override;
        void uploadTexture(TextureHandle, uint32_t mip, const void* data, uint32_t rowPitch) override;
        void* mapBuffer(BufferHandle, uint64_t offset, uint64_t size, uint32_t mapFlags) override;
        void unmapBuffer(BufferHandle) override;
        void flushMappedRange(BufferHandle, uint64_t offset, uint64_t size) override;

        void beginFrame() override;
        void endFrame() override;
        void present() override;

        void bindPipeline(PipelineHandle) override;
        void bindVertexBuffer(uint32_t slot, BufferHandle, uint64_t offset) override;
        void bindIndexBuffer(BufferHandle, uint64_t offset) override;
        void bindUniformBuffer(uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindShaderStorageBuffer(
            uint32_t set, uint32_t binding, BufferHandle, uint64_t offset, uint64_t size) override;
        void bindTexture(uint32_t set, uint32_t binding, TextureHandle) override;
        void setViewport(const Viewport&) override;
        void setScissor(const Scissor&) override;
        void setLineWidth(float width) override;

        void setUniformMatrix3(const char* name, const float* data) override;
        void setUniformMatrix4(const char* name, const float* data) override;
        void setUniformFloat(const char* name, float value) override;
        void setUniformInt(const char* name, int32_t value) override;
        void setUniformVec2(const char* name, const float* data) override;
        void setUniformVec3(const char* name, const float* data) override;
        void setUniformVec4(const char* name, const float* data) override;

        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override;
        void drawIndexed(uint32_t indexCount,
            uint32_t instanceCount,
            uint32_t firstIndex,
            int32_t vertexOffset,
            uint32_t firstInstance) override;

        /// 诊断：读回帧缓冲指定区域，统计与背景色差异超过容差的像素数
        uint32_t readRegionNonBgPixels(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const float bg[4], int tol);
        void drawIndirect(BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;
        void drawIndexedIndirect(
            BufferHandle indirectBuffer, uint64_t offset, uint32_t drawCount, uint32_t stride) override;

        void dispatchCompute(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ) override;
        void memoryBarrier(uint32_t barrierFlags) override;

        void setClearColor(float r, float g, float b, float a) override;
        void clear(uint32_t flags) override;
        void enableDepthTest(bool enable) override;
        void enableBlend(bool enable) override;

        void resize(uint32_t width, uint32_t height) override;

        uint64_t getGPUMemoryUsage() const override;
        void* getNativeContext() override;

    private:
        uint32_t compileShader(uint32_t type, const char* source);
        uint32_t compileShaderSPIRV(
            ShaderStage stage, const uint32_t* spirvWords, uint32_t wordCount, const char* entryPoint);
        uint32_t linkProgram(uint32_t vs, uint32_t fs);
        uint32_t createComputeProgram(const ShaderModuleDesc* modules, uint32_t count);
        uint32_t shaderStageToGL(ShaderStage stage) const;
        uint32_t topologyToGL(PrimitiveTopology topo) const;
        uint32_t formatToGLInternal(Format fmt) const;
        uint32_t formatToGLFormat(Format fmt) const;
        uint32_t formatToGLType(Format fmt) const;
        uint32_t getFormatBytesPerPixel(Format fmt) const;
        // 计算顶点格式的跨度（字节数）
        uint32_t vertexFormatStride(VertexFormat fmt) const;

        void configureVertexAttribs(GLFuncs* g, PrimitiveTopology topo, VertexFormat fmt, uint64_t baseOffset);

        BufferHandle allocBufferHandle();
        TextureHandle allocTextureHandle();
        PipelineHandle allocPipelineHandle();

        void* m_nativeContext = nullptr;
        uint32_t m_vao = 0;
        uint32_t m_width = 0;
        uint32_t m_height = 0;
        bool m_initialized = false;
        float m_clearColor[4] = { 0.f, 0.f, 0.f, 1.f };

        /// GPU 支持的线宽范围（通过 GL_LINE_WIDTH_RANGE 查询）
        /// macOS CoreProfile 下通常为 [1, 1]，Windows 可能支持更宽
        float m_minLineWidth = 1.0f;
        float m_maxLineWidth = 1.0f;

        std::vector<GLBufferEntry> m_buffers;
        std::vector<uint32_t> m_bufferFreeList;

        std::vector<GLTextureEntry> m_textures;
        std::vector<uint32_t> m_textureFreeList;

        std::vector<GLPipelineEntry> m_pipelines;
        std::vector<uint32_t> m_pipelineFreeList;

        PipelineHandle m_currentPipeline = NullHandle;
        BufferHandle m_currentVBOs[4] = { NullHandle, NullHandle, NullHandle, NullHandle };
        uint64_t m_currentVBOOffsets[4] = { 0, 0, 0, 0 };
        BufferHandle m_currentIBO = NullHandle;
        uint64_t m_currentIBOOffset = 0;
        bool m_depthTestEnabled = false;
        bool m_blendEnabled = false;
    };

}  // namespace render::rhi
