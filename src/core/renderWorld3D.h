#pragma once

#include "render/render_types.h"

#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace render
{
    struct EntityEntry3D
    {
        EntityId entityId = 0;
        uint32_t vertexOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexOffset = 0;
        uint32_t indexCount = 0;
        uint16_t primitiveType = 0;
        uint16_t materialIndex = 0;
        uint32_t flags = 0;
        float bboxMin[3] = { 0, 0, 0 };
        float bboxMax[3] = { 0, 0, 0 };
        bool dirty = true;
    };

    class RenderWorld3D
    {
    public:
        RenderWorld3D() = default;
        ~RenderWorld3D();

        RenderWorld3D(const RenderWorld3D&) = delete;
        RenderWorld3D& operator=(const RenderWorld3D&) = delete;

        bool initialize(uint32_t initialVertexCapacity = 65536, uint32_t initialIndexCapacity = 65536);
        void shutdown();

        void addEntity(EntityId id,
            const VertexP3N3* vertices,
            uint32_t vertexCount,
            const uint32_t* indices,
            uint32_t indexCount,
            uint16_t materialIdx);
        void removeEntity(EntityId id);
        void clear();

        const EntityEntry3D* getEntityEntries() const;
        uint32_t getEntityCount() const;
        const VertexP3N3* getVertexData() const;
        uint32_t getTotalVertexCount() const;
        const uint32_t* getIndexData() const;
        uint32_t getTotalIndexCount() const;

        bool hasDirtyEntities() const;
        void clearDirtyFlags();

    private:
        struct Impl;
        Impl* m_impl;
    };

}  // namespace render
