#pragma once

#include "render/render_types.h"
#include "../core/slot_map.h"
#include "../core/arena.h"
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <cmath>

namespace render {
namespace core {

class RenderWorld {
private:
    struct EntityEntry {
        uint32_t vertexOffset;
        uint32_t vertexCount;
        uint16_t primitiveType;
        uint16_t materialIndex;
        uint32_t flags;
        float bbox[4];
        bool dirty;
    };

    struct QuadTreeNode {
        float minX, minY, maxX, maxY;
        uint32_t firstChild;
        uint32_t firstEntity;
        uint32_t entityCount;
        bool isLeaf;
    };

    struct MaterialEntry {
        MaterialDesc desc;
    };

    static constexpr uint32_t kQuadTreeMaxEntities = 64;
    static constexpr uint32_t kQuadTreeMaxDepth = 8;
    static constexpr uint32_t kEntityFlagHidden = 1u << 0;

    void computeBBox(const VertexP3C3* verts, uint32_t count,
                     float* outMinX, float* outMinY, float* outMaxX, float* outMaxY) const;
    void rebuildQuadTree();
    void insertQuadTree(uint32_t nodeIdx, uint32_t entityDenseIdx, uint32_t depth);
    bool bboxIntersects(float aMinX, float aMinY, float aMaxX, float aMaxY,
                        float bMinX, float bMinY, float bMaxX, float bMaxY) const;
    bool isEntityVisible(uint32_t denseIdx, const float frustum[4]) const;

    SlotMap<uint64_t, EntityEntry> m_entities;
    std::unordered_map<EntityId, uint64_t> m_entityKeyMap;
    std::unordered_map<EntityId, std::vector<VertexP3C3>> m_entityVertices;
    std::vector<VertexP3C3> m_vertexPool;
    std::vector<uint32_t> m_dirtyList;
    bool m_quadTreeDirty;
    std::vector<QuadTreeNode> m_quadTree;
    std::vector<uint32_t> m_quadTreeEntities;
    std::vector<uint32_t> m_quadTreeEntityNext;
    std::vector<MaterialEntry> m_materials;
    mutable std::vector<uint32_t> m_visibleResult;

public:
    bool initialize();
    void shutdown();

    void addEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
                   PrimitiveType type, uint16_t materialIdx);
    void modifyEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
                      uint16_t materialIdx);
    void removeEntity(EntityId id);
    void setEntityVisibility(EntityId id, bool visible);

    uint16_t addMaterial(const MaterialDesc* desc);
    void updateMaterial(uint16_t idx, const MaterialDesc* desc);

    void queryVisible(const float viewMatrix[9], float viewWidth, float viewHeight,
                      uint32_t* outIndices, uint32_t* outCount, uint32_t maxOut) const;

    void update();

    const VertexP3C3* getVertexData() const { return m_vertexPool.data(); }
    uint32_t getTotalVertexCount() const { return static_cast<uint32_t>(m_vertexPool.size()); }
    const EntityEntry* getEntityEntries() const;
    uint32_t getEntityCount() const;
    const MaterialEntry* getMaterials() const { return m_materials.data(); }
    uint16_t getMaterialCount() const { return static_cast<uint16_t>(m_materials.size()); }
    uint32_t getVisibleCount() const { return static_cast<uint32_t>(m_visibleResult.size()); }
    void getVisibleIndices(const uint32_t** outIndices, uint32_t* outCount) const {
        *outIndices = m_visibleResult.data();
        *outCount = static_cast<uint32_t>(m_visibleResult.size());
    }
};

inline bool RenderWorld::bboxIntersects(float aMinX, float aMinY, float aMaxX, float aMaxY,
                                        float bMinX, float bMinY, float bMaxX, float bMaxY) const {
    return aMinX <= bMaxX && aMaxX >= bMinX && aMinY <= bMaxY && aMaxY >= bMinY;
}

} // namespace core
} // namespace render
