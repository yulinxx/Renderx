#pragma once
#include <cstdint>

namespace render::rhi {

enum class Format : uint8_t { RGBA8, RGBA32F, RG32F, R32F, D32F, D24S8, R8 };

enum class BufferUsage : uint8_t {
    Vertex, Index, Uniform, Indirect, ShaderVisible, Staging
};

enum class MemoryType : uint8_t {
    GPU_Only, CPU_Visible, GPU_CPU_Coherent
};

enum class PrimitiveTopology : uint8_t {
    PointList, LineList, LineStrip, LineLoop, TriangleList, TriangleStrip, TriangleFan
};

enum class CompareFunc : uint8_t { Never, Less, Equal, LessEqual, Greater, Always };
enum class BlendFactor : uint8_t { Zero, One, SrcAlpha, OneMinusSrcAlpha };
enum class BlendOp : uint8_t { Add };

struct BufferDesc {
    uint64_t    size;
    BufferUsage usage;
    MemoryType  memory;
    const char* debugName;
};

struct TextureDesc {
    uint32_t width, height;
    Format   format;
    uint32_t mipLevels;
    const char* debugName;
};

struct PipelineDesc {
    PrimitiveTopology topology;
    const char*  vertexShader;
    const char*  fragmentShader;
    const char*  computeShader;
    bool         depthTest;
    bool         depthWrite;
    bool         blendEnable;
    BlendFactor  srcBlend;
    BlendFactor  dstBlend;
    CompareFunc  depthFunc;
};

using BufferHandle   = uint64_t;
using TextureHandle  = uint64_t;
using PipelineHandle = uint64_t;

constexpr uint64_t NullHandle = 0;

struct Viewport { float x, y, w, h; };
struct Scissor  { int32_t x, y; uint32_t w, h; };

struct VertexAttrib {
    uint32_t location;
    uint32_t offset;
    uint32_t stride;
    bool     normalized;
};

}
