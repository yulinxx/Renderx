#include "render_world.h"
#include <cfloat>
#include <algorithm>

namespace render {
namespace core {

bool RenderWorld::initialize() {
    m_vertexPool.reserve(1024 * 1024);
    m_entities.reserve(100000);
    m_quadTreeDirty = false;
    m_dirtyList.clear();
    return true;
}

void RenderWorld::shutdown() {
    m_vertexPool.clear();
    m_vertexPool.shrink_to_fit();
    m_entities.clear();
    m_entityKeyMap.clear();
    m_entityVertices.clear();
    m_dirtyList.clear();
    m_quadTree.clear();
    m_quadTreeEntities.clear();
    m_quadTreeEntityNext.clear();
    m_materials.clear();
    m_visibleResult.clear();
    m_quadTreeDirty = false;
}

void RenderWorld::addEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
                            PrimitiveType type, uint16_t materialIdx) {
    EntityEntry entry;
    entry.vertexOffset = 0;
    entry.vertexCount = vertexCount;
    entry.primitiveType = static_cast<uint16_t>(type);
    entry.materialIndex = materialIdx;
    entry.flags = 0;
    entry.dirty = true;
    computeBBox(vertices, vertexCount, &entry.bbox[0], &entry.bbox[1],
                &entry.bbox[2], &entry.bbox[3]);

    uint64_t key = m_entities.insert(entry);
    m_entityKeyMap[id] = key;
    m_entityVertices[id].assign(vertices, vertices + vertexCount);
    m_dirtyList.push_back(m_entities.size() - 1);
    m_quadTreeDirty = true;
}

void RenderWorld::modifyEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
                               uint16_t materialIdx) {
    auto it = m_entityKeyMap.find(id);
    if (it == m_entityKeyMap.end()) return;

    EntityEntry* entry = m_entities.find(it->second);
    if (!entry) return;

    entry->vertexCount = vertexCount;
    entry->materialIndex = materialIdx;
    entry->dirty = true;
    computeBBox(vertices, vertexCount, &entry->bbox[0], &entry->bbox[1],
                &entry->bbox[2], &entry->bbox[3]);

    m_entityVertices[id].assign(vertices, vertices + vertexCount);
    m_quadTreeDirty = true;
}

void RenderWorld::removeEntity(EntityId id) {
    auto it = m_entityKeyMap.find(id);
    if (it == m_entityKeyMap.end()) return;

    m_entities.erase(it->second);
    m_entityKeyMap.erase(it);
    m_entityVertices.erase(id);
    m_quadTreeDirty = true;
}

void RenderWorld::setEntityVisibility(EntityId id, bool visible) {
    auto it = m_entityKeyMap.find(id);
    if (it == m_entityKeyMap.end()) return;

    EntityEntry* entry = m_entities.find(it->second);
    if (!entry) return;

    if (visible)
        entry->flags &= ~kEntityFlagHidden;
    else
        entry->flags |= kEntityFlagHidden;
}

uint16_t RenderWorld::addMaterial(const MaterialDesc* desc) {
    uint16_t idx = static_cast<uint16_t>(m_materials.size());
    MaterialEntry me;
    me.desc = *desc;
    m_materials.push_back(me);
    return idx;
}

void RenderWorld::updateMaterial(uint16_t idx, const MaterialDesc* desc) {
    if (idx < static_cast<uint16_t>(m_materials.size()))
        m_materials[idx].desc = *desc;
}

void RenderWorld::queryVisible(const float viewMatrix[9], float viewWidth, float viewHeight,
                               uint32_t* outIndices, uint32_t* outCount, uint32_t maxOut) const {
    if (m_quadTreeDirty)
        const_cast<RenderWorld*>(this)->rebuildQuadTree();

    m_visibleResult.clear();

    if (m_quadTree.empty() || m_entities.size() == 0) {
        if (outIndices) *outIndices = 0;
        *outCount = 0;
        return;
    }

    float m00 = viewMatrix[0], m01 = viewMatrix[1], m02 = viewMatrix[2];
    float m10 = viewMatrix[3], m11 = viewMatrix[4], m12 = viewMatrix[5];
    float m20 = viewMatrix[6], m21 = viewMatrix[7], m22 = viewMatrix[8];

    float det = m00 * (m11 * m22 - m12 * m21)
              - m01 * (m10 * m22 - m12 * m20)
              + m02 * (m10 * m21 - m11 * m20);

    float frustum[4];

    if (std::abs(det) < 1e-10f) {
        frustum[0] = -FLT_MAX;
        frustum[1] = -FLT_MAX;
        frustum[2] = FLT_MAX;
        frustum[3] = FLT_MAX;
    } else {
        float invDet = 1.0f / det;
        float inv[9];
        inv[0] = (m11 * m22 - m12 * m21) * invDet;
        inv[1] = (m02 * m21 - m01 * m22) * invDet;
        inv[2] = (m01 * m12 - m02 * m11) * invDet;
        inv[3] = (m12 * m20 - m10 * m22) * invDet;
        inv[4] = (m00 * m22 - m02 * m20) * invDet;
        inv[5] = (m02 * m10 - m00 * m12) * invDet;
        inv[6] = (m10 * m21 - m11 * m20) * invDet;
        inv[7] = (m01 * m20 - m00 * m21) * invDet;
        inv[8] = (m00 * m11 - m01 * m10) * invDet;

        static const float kCorners[4][2] = {
            {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}
        };

        frustum[0] = FLT_MAX;  frustum[1] = FLT_MAX;
        frustum[2] = -FLT_MAX; frustum[3] = -FLT_MAX;

        for (int i = 0; i < 4; i++) {
            float cx = kCorners[i][0], cy = kCorners[i][1];
            float wx = inv[0] * cx + inv[1] * cy + inv[2];
            float wy = inv[3] * cx + inv[4] * cy + inv[5];
            float w  = inv[6] * cx + inv[7] * cy + inv[8];
            if (std::abs(w) < 1e-10f) continue;
            float worldX = wx / w;
            float worldY = wy / w;
            frustum[0] = std::min(frustum[0], worldX);
            frustum[1] = std::min(frustum[1], worldY);
            frustum[2] = std::max(frustum[2], worldX);
            frustum[3] = std::max(frustum[3], worldY);
        }
    }

    std::vector<uint32_t> stack;
    stack.push_back(0);

    while (!stack.empty()) {
        uint32_t nodeIdx = stack.back();
        stack.pop_back();

        const QuadTreeNode& node = m_quadTree[nodeIdx];

        if (!bboxIntersects(node.minX, node.minY, node.maxX, node.maxY,
                            frustum[0], frustum[1], frustum[2], frustum[3]))
            continue;

        if (node.isLeaf) {
            uint32_t iter = node.firstEntity;
            while (iter != UINT32_MAX) {
                uint32_t denseIdx = m_quadTreeEntities[iter];
                if (isEntityVisible(denseIdx, frustum)) {
                    const EntityEntry& e = *(m_entities.begin() + denseIdx);
                    if (!(e.flags & kEntityFlagHidden))
                        m_visibleResult.push_back(denseIdx);
                }
                iter = m_quadTreeEntityNext[iter];
            }
        } else {
            for (uint32_t c = 0; c < 4; c++)
                stack.push_back(node.firstChild + c);
        }
    }

    uint32_t count = static_cast<uint32_t>(
        std::min(m_visibleResult.size(), static_cast<size_t>(maxOut)));
    if (outIndices && count > 0)
        std::memcpy(outIndices, m_visibleResult.data(), count * sizeof(uint32_t));
    *outCount = count;
}

void RenderWorld::update() {
    m_vertexPool.clear();
    uint32_t offset = 0;

    for (auto& kv : m_entityKeyMap) {
        EntityEntry* entry = m_entities.find(kv.second);
        if (!entry) continue;

        auto& verts = m_entityVertices[kv.first];
        entry->vertexOffset = offset;
        entry->vertexCount = static_cast<uint32_t>(verts.size());
        m_vertexPool.insert(m_vertexPool.end(), verts.begin(), verts.end());
        offset += entry->vertexCount;
        entry->dirty = false;
    }

    m_dirtyList.clear();
}

void RenderWorld::computeBBox(const VertexP3C3* verts, uint32_t count,
                              float* outMinX, float* outMinY,
                              float* outMaxX, float* outMaxY) const {
    if (count == 0) {
        *outMinX = *outMinY = *outMaxX = *outMaxY = 0.0f;
        return;
    }

    *outMinX = verts[0].px; *outMinY = verts[0].py;
    *outMaxX = verts[0].px; *outMaxY = verts[0].py;

    for (uint32_t i = 1; i < count; i++) {
        *outMinX = std::min(*outMinX, verts[i].px);
        *outMinY = std::min(*outMinY, verts[i].py);
        *outMaxX = std::max(*outMaxX, verts[i].px);
        *outMaxY = std::max(*outMaxY, verts[i].py);
    }
}

void RenderWorld::rebuildQuadTree() {
    m_quadTree.clear();
    m_quadTreeEntities.clear();
    m_quadTreeEntityNext.clear();

    if (m_entities.size() == 0) {
        m_quadTreeDirty = false;
        return;
    }

    float sceneMinX = FLT_MAX, sceneMinY = FLT_MAX;
    float sceneMaxX = -FLT_MAX, sceneMaxY = -FLT_MAX;

    for (const auto& entry : m_entities) {
        sceneMinX = std::min(sceneMinX, entry.bbox[0]);
        sceneMinY = std::min(sceneMinY, entry.bbox[1]);
        sceneMaxX = std::max(sceneMaxX, entry.bbox[2]);
        sceneMaxY = std::max(sceneMaxY, entry.bbox[3]);
    }

    float pad = std::max(sceneMaxX - sceneMinX, sceneMaxY - sceneMinY) * 0.01f;
    if (pad < 1.0f) pad = 1.0f;
    sceneMinX -= pad; sceneMinY -= pad;
    sceneMaxX += pad; sceneMaxY += pad;

    m_quadTree.push_back({sceneMinX, sceneMinY, sceneMaxX, sceneMaxY,
                          UINT32_MAX, UINT32_MAX, 0, true});

    uint32_t idx = 0;
    for (const auto& entry : m_entities) {
        insertQuadTree(0, idx, 0);
        idx++;
    }

    m_quadTreeDirty = false;
}

void RenderWorld::insertQuadTree(uint32_t nodeIdx, uint32_t entityDenseIdx, uint32_t depth) {
    QuadTreeNode& node = m_quadTree[nodeIdx];
    const EntityEntry& entry = *(m_entities.begin() + entityDenseIdx);

    if (!bboxIntersects(node.minX, node.minY, node.maxX, node.maxY,
                        entry.bbox[0], entry.bbox[1], entry.bbox[2], entry.bbox[3]))
        return;

    if (node.isLeaf) {
        if (node.entityCount < kQuadTreeMaxEntities || depth >= kQuadTreeMaxDepth) {
            uint32_t slotIdx = static_cast<uint32_t>(m_quadTreeEntities.size());
            m_quadTreeEntities.push_back(entityDenseIdx);
            m_quadTreeEntityNext.push_back(node.firstEntity);
            node.firstEntity = slotIdx;
            node.entityCount++;
            return;
        }

        float midX = (node.minX + node.maxX) * 0.5f;
        float midY = (node.minY + node.maxY) * 0.5f;

        uint32_t firstChild = static_cast<uint32_t>(m_quadTree.size());
        m_quadTree.push_back({node.minX, node.minY, midX, midY,
                              UINT32_MAX, UINT32_MAX, 0, true});
        m_quadTree.push_back({midX, node.minY, node.maxX, midY,
                              UINT32_MAX, UINT32_MAX, 0, true});
        m_quadTree.push_back({midX, midY, node.maxX, node.maxY,
                              UINT32_MAX, UINT32_MAX, 0, true});
        m_quadTree.push_back({node.minX, midY, midX, node.maxY,
                              UINT32_MAX, UINT32_MAX, 0, true});

        std::vector<uint32_t> existing;
        uint32_t iter = node.firstEntity;
        while (iter != UINT32_MAX) {
            existing.push_back(m_quadTreeEntities[iter]);
            iter = m_quadTreeEntityNext[iter];
        }

        node.firstChild = firstChild;
        node.isLeaf = false;
        node.firstEntity = UINT32_MAX;
        node.entityCount = 0;

        for (uint32_t eidx : existing) {
            for (uint32_t c = 0; c < 4; c++)
                insertQuadTree(firstChild + c, eidx, depth + 1);
        }

        for (uint32_t c = 0; c < 4; c++)
            insertQuadTree(firstChild + c, entityDenseIdx, depth + 1);
        return;
    }

    for (uint32_t c = 0; c < 4; c++)
        insertQuadTree(node.firstChild + c, entityDenseIdx, depth + 1);
}

bool RenderWorld::isEntityVisible(uint32_t denseIdx, const float frustum[4]) const {
    const EntityEntry& entry = *(m_entities.begin() + denseIdx);
    return bboxIntersects(entry.bbox[0], entry.bbox[1], entry.bbox[2], entry.bbox[3],
                          frustum[0], frustum[1], frustum[2], frustum[3]);
}

const RenderWorld::EntityEntry* RenderWorld::getEntityEntries() const {
    return m_entities.dense_data();
}

uint32_t RenderWorld::getEntityCount() const {
    return m_entities.size();
}

} // namespace core
} // namespace render
