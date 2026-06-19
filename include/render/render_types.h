#pragma once

#include <cstdint>
#include <cstddef>

namespace render {

using EntityId = uint64_t;
using MeshId   = uint64_t;

constexpr EntityId INVALID_ENTITY_ID = 0;
constexpr MeshId   INVALID_MESH_ID   = 0;

enum class PrimitiveType : uint8_t
{
    PointList     = 0,
    LineList      = 1,
    LineStrip     = 2,
    LineLoop      = 3,
    TriangleList  = 4,
    TriangleStrip = 5,
    TriangleFan   = 6,
};

constexpr uint32_t PRIMITIVE_TYPE_COUNT = 7;

struct VertexP3C3
{
    float px, py, pz;
    float cr, cg, cb;
};

static_assert(sizeof(VertexP3C3) == 24, "VertexP3C3 must be 24 bytes");

struct VertexP3N3
{
    float px, py, pz;
    float nx, ny, nz;
};

static_assert(sizeof(VertexP3N3) == 24, "VertexP3N3 must be 24 bytes");

enum class UpdateOp : uint8_t
{
    Add    = 0,
    Modify = 1,
    Remove = 2,
};

struct EntityUpdate
{
    UpdateOp     op;
    uint8_t      _pad[3];
    EntityId     entityId;
    uint32_t     vertexCount;
    uint16_t     primitiveType;
    uint16_t     materialIndex;
};

struct MaterialDesc
{
    float lineWidth;
    float pointSize;
    float color[4];
    uint32_t flags;
};

enum class EntityFlags : uint32_t
{
    None       = 0,
    Visible    = 1 << 0,
    Selected   = 1 << 1,
    Highlighted = 1 << 2,
};

struct EntityDesc
{
    EntityId   entityId;
    uint32_t   vertexOffset;
    uint32_t   vertexCount;
    uint16_t   primitiveType;
    uint16_t   materialIndex;
    uint32_t   flags;
    float      boundingBox[4];
};

struct MeshDesc
{
    uint32_t indexOffset;
    uint32_t indexCount;
    uint32_t vertexOffset;
    uint32_t vertexCount;
};

struct InstanceDesc
{
    float    modelMatrix[16];
    uint32_t meshIndex;
    uint32_t materialIndex;
    uint32_t flags;
    uint32_t _pad;
};

struct DrawIndirectCmd
{
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t baseInstance;
};

struct DrawIndexedIndirectCmd
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t baseInstance;
};

enum class BackendType : int
{
    OpenGL = 0,
    Vulkan = 1,
    Metal  = 2,
    Null   = 3,
};

struct DeviceDesc
{
    BackendType backend;
    bool        debugLayer;
    uint8_t     _pad[2];
    void*       nativeWindowHandle;
    uint32_t    width;
    uint32_t    height;
};

struct ViewDesc2D
{
    float viewMatrix[9];
    float viewWidth;
    float viewHeight;
};

struct ViewDesc3D
{
    float viewMatrix[16];
    float projMatrix[16];
};

struct OverlayData
{
    float  crosshairWorld[2];
    int32_t crosshairVisible;
    float  snapWorld[2];
    int32_t snapVisible;
    float  snapColor[4];
    float  mouseWorld[2];
};

struct TextItem
{
    const char* text;
    float       x, y;
    int32_t     coordMode;
    int32_t     hAlign;
    int32_t     vAlign;
    int32_t     fontSize;
    float       color[4];
    float       rotationDeg;
    float       zOrder;
};

struct TextItemList
{
    const TextItem* items;
    uint32_t        count;
};

struct BBox2f
{
    float minX, minY, maxX, maxY;
};

}
