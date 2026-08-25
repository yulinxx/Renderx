#pragma once

#include "RenderTypes.h"
#include <cstdint>

namespace Render
{
    namespace RHI
    {
        class IDevice;
    }
}

#ifndef RENDER_API
    #if defined(_WIN32) || defined(_WIN64)
        #ifdef RENDER_EXPORTS
            #define RENDER_API __declspec(dllexport)
        #else
            #define RENDER_API __declspec(dllimport)
        #endif
    #elif defined(__GNUC__) || defined(__clang__)
        #ifdef RENDER_EXPORTS
            #define RENDER_API __attribute__((visibility("default")))
        #else
            #define RENDER_API
        #endif
    #else
        #define RENDER_API
    #endif
#endif

namespace Render
{
    namespace RT
    {

        using RuntimeHandle = uint64_t;
        using SessionHandle = uint64_t;
        using BufferHandle = uint64_t;
        using PipelineHandle = uint64_t;
        using TextureHandle = uint64_t;

        enum class Backend : int
        {
            OpenGL = 0,
            Vulkan = 1,
            Metal = 2,
            Null = 3,
        };

        enum class RenderSpace : uint8_t
        {
            World = 0,
            Screen = 1,
        };

        enum class RTPrimitiveTopology : uint8_t
        {
            Points = 0,
            Lines = 1,
            LineStrip = 2,
            LineLoop = 3,
            Triangles = 4,
            TriangleStrip = 5,
            TriangleFan = 6,
        };

        enum class RTVertexFormat : uint8_t
        {
            P3C3 = 0,
            P3C4 = 1,
            P3N3 = 2,
            P2T2C4 = 6,
        };

        enum class RTBlendFactor : uint8_t
        {
            Zero = 0,
            One = 1,
            SrcAlpha = 2,
            OneMinusSrcAlpha = 3,
        };

        enum class RTDepthFunc : uint8_t
        {
            Always = 0,
            Less = 1,
            LessEqual = 2,
            Greater = 3,
        };

        struct RuntimeDesc
        {
            Backend backend = Backend::OpenGL;
            bool debugLayer = false;
            void* nativeWindowHandle = nullptr;
            uint32_t width = 1;
            uint32_t height = 1;
            const char* shaderDirectory = nullptr;
            uint64_t transientBufferSize = 64 * 1024 * 1024;
            /// 若非空：复用已有的 RHI 设备（不创建/不拥有），帧生命周期由外部（旧渲染帧）负责。
            /// 此时 Runtime 只负责绘制命令，不调用 beginFrame/clear/present。
            RHI::IDevice* existingDevice = nullptr;
        };

        struct SessionDesc
        {
            RuntimeHandle runtime = 0;
            void* nativeWindowHandle = nullptr;
            uint32_t width = 1;
            uint32_t height = 1;
            float clearColor[4] = { 0.94f, 0.94f, 0.94f, 1.0f };
        };

        struct RTDrawCommand
        {
            uint64_t vertexBufferHandle = 0;
            uint64_t indexBufferHandle = 0;
            uint32_t vertexOffset = 0;
            uint32_t vertexCount = 0;
            uint32_t indexOffset = 0;
            uint32_t indexCount = 0;
            uint64_t sortKey = 0;
            uint64_t userData = 0;
            uint64_t textureHandle = 0;
            RTPrimitiveTopology topology = RTPrimitiveTopology::LineStrip;
            RenderSpace space = RenderSpace::World;
            uint8_t vertexFormat = 0;
            uint8_t indexType = 0;
            uint16_t pipelineIndex = 0;
            uint16_t materialIndex = 0;
            uint32_t instanceCount = 1;
            uint32_t firstInstance = 0;
            float lineWidth = 0.0f;
            float pointSize = 0.0f;
        };

        static_assert(sizeof(RTDrawCommand) == 80, "RTDrawCommand must be 80 bytes");

        struct RTDrawPacket
        {
            const RTDrawCommand* commands = nullptr;
            uint32_t commandCount = 0;
            uint32_t _pad = 0;
            float viewMatrix[16] = {};
            float viewport[4] = {};
            uint64_t frameId = 0;
            uint32_t enableCulling = 0;
            uint32_t _pad2 = 0;
        };

        struct RTTransientAlloc
        {
            BufferHandle handle = 0;
            uint32_t offset = 0;
            uint32_t size = 0;
            void* cpuPtr = nullptr;
        };

        struct RTVisibilityResult
        {
            uint32_t* indices = nullptr;
            uint32_t count = 0;
            uint32_t maxCount = 0;
        };

        struct RTStats
        {
            uint32_t drawCallCount = 0;
            uint32_t triangleCount = 0;
            uint32_t lineCount = 0;
            uint32_t pointCount = 0;
            uint64_t gpuMemoryBytes = 0;
        };

        struct RTBufferDesc
        {
            uint64_t size = 0;
            uint32_t usageFlags = 0;
            uint32_t memoryFlags = 0;
        };

        struct RTPipelineDesc
        {
            RTPrimitiveTopology topology = RTPrimitiveTopology::LineStrip;
            RTVertexFormat vertexFormat = RTVertexFormat::P3C3;
            uint8_t depthTest = 0;
            uint8_t depthWrite = 0;
            uint8_t blendEnable = 0;
            RTBlendFactor srcBlend = RTBlendFactor::SrcAlpha;
            RTBlendFactor dstBlend = RTBlendFactor::OneMinusSrcAlpha;
            RTDepthFunc depthFunc = RTDepthFunc::Less;
            const char* vertexShader = nullptr;
            const char* fragmentShader = nullptr;
        };

        struct RTTextureDesc
        {
            uint32_t width = 0;
            uint32_t height = 0;
            const uint8_t* rgba = nullptr;
            uint32_t rgbaBytes = 0;
        };

        enum class DefaultPipeline : uint8_t
        {
            WorldLine = 0,
            WorldTri = 1,
            WorldPoint = 2,
            ScreenLine = 3,
            ScreenTri = 4,
            ScreenPoint = 5,
            ScreenTextured = 6,
            // P3C4 (alpha-aware) variants — used by overlay/selection primitives
            WorldLine4 = 7,
            WorldTri4 = 8,
            WorldPoint4 = 9,
            ScreenLine4 = 10,
            ScreenTri4 = 11,
            ScreenPoint4 = 12,
            Count = 13,
        };

        extern "C"
        {

        RENDER_API RuntimeHandle runtimeCreate(const RuntimeDesc* desc);
        RENDER_API void runtimeDestroy(RuntimeHandle runtime);

        RENDER_API BufferHandle runtimeCreateBuffer(RuntimeHandle runtime, const RTBufferDesc* desc);
        RENDER_API void runtimeDestroyBuffer(RuntimeHandle runtime, BufferHandle buffer);
        RENDER_API void runtimeUploadBuffer(RuntimeHandle runtime, BufferHandle buffer,
            uint64_t offset, uint64_t size, const void* data);

        RENDER_API PipelineHandle runtimeCreatePipeline(RuntimeHandle runtime, const RTPipelineDesc* desc);
        RENDER_API uint16_t runtimeGetDefaultPipeline(RuntimeHandle runtime, DefaultPipeline kind);

        RENDER_API TextureHandle runtimeCreateTexture(RuntimeHandle runtime, const RTTextureDesc* desc);
        RENDER_API void runtimeDestroyTexture(RuntimeHandle runtime, TextureHandle texture);
        RENDER_API void runtimeUpdateTexture(RuntimeHandle runtime, TextureHandle texture, const RTTextureDesc* desc);

        RENDER_API uint16_t runtimeAddMaterial(RuntimeHandle runtime, const MaterialDesc* desc);
        RENDER_API void runtimeUpdateMaterial(RuntimeHandle runtime, uint16_t index, const MaterialDesc* desc);

        RENDER_API void runtimeFrameBegin(RuntimeHandle runtime);
        RENDER_API void runtimeAllocTransient(RuntimeHandle runtime, uint64_t size, RTTransientAlloc* out);
        RENDER_API void runtimeFrameEnd(RuntimeHandle runtime);

        RENDER_API uint64_t rtMakeSortKey(uint8_t layer, uint8_t transparent, uint16_t depth, uint16_t seq);

        RENDER_API SessionHandle sessionCreate(const SessionDesc* desc);
        RENDER_API void sessionDestroy(SessionHandle session);
        RENDER_API void sessionResize(SessionHandle session, uint32_t width, uint32_t height);
        RENDER_API void sessionSetClearColor(SessionHandle session, float r, float g, float b, float a);
        RENDER_API void sessionSetViewMatrix(SessionHandle session, const float viewMatrix[16]);

        RENDER_API void sessionSubmitDrawCommands(SessionHandle session, const RTDrawPacket* packet);
        RENDER_API void sessionQueryVisibility(SessionHandle session,
            const float* aabbs, uint32_t aabbCount, const float viewBounds[4], RTVisibilityResult* out);
        RENDER_API void sessionPresent(SessionHandle session);
        RENDER_API void sessionGetStats(SessionHandle session, RTStats* out);

        }  // extern "C"

    }  // namespace RT
}  // namespace Render
