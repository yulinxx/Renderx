#include "render_world.h"
#include <cfloat>
#include <algorithm>
#include "Log/SyLogger.h"

namespace render
{
    namespace core
    {
        bool RenderWorld::initialize()
        {
            m_vertexPool.reserve(1024 * 1024);
            m_entities.reserve(100000);
            m_freeList.reserve(1024);
            m_quadTreeDirty = false;
            m_vertexPoolResized = false;
            m_changeCount = 0;
            m_generation = 0;
            std::memset(m_lastViewMatrix, 0, sizeof(m_lastViewMatrix));
            m_dirtyList.clear();

            MaterialEntry defaultMaterial;
            defaultMaterial.desc.lineWidth = 1.0f;
            defaultMaterial.desc.pointSize = 1.0f;
            defaultMaterial.desc.color[0] = 0.0f;
            defaultMaterial.desc.color[1] = 0.0f;
            defaultMaterial.desc.color[2] = 0.0f;
            defaultMaterial.desc.color[3] = 1.0f;
            m_materials.push_back(defaultMaterial);

            return true;
        }

        void RenderWorld::shutdown()
        {
            m_vertexPool.clear();
            m_vertexPool.shrink_to_fit();
            m_entities.clear();
            m_entityKeyMap.clear();
            m_freeList.clear();
            m_dirtyList.clear();
            m_quadTree.clear();
            m_quadTreeEntities.clear();
            m_quadTreeEntityNext.clear();
            m_materials.clear();
            m_visibleResult.clear();
            m_quadTreeDirty = false;
            m_vertexPoolResized = false;
            m_changeCount = 0;
        }

        uint32_t RenderWorld::allocateVertexSpace(uint32_t vertexCount)
        {
            for (size_t i = 0; i < m_freeList.size(); i += 2)
            {
                uint32_t offset = m_freeList[i];
                uint32_t size = m_freeList[i + 1];
                if (size >= vertexCount)
                {
                    m_freeList.erase(m_freeList.begin() + i, m_freeList.begin() + i + 2);
                    if (size > vertexCount)
                    {
                        m_freeList.push_back(offset + vertexCount);
                        m_freeList.push_back(size - vertexCount);
                    }
                    return offset;
                }
            }

            uint32_t offset = static_cast<uint32_t>(m_vertexPool.size());
            m_vertexPool.resize(offset + vertexCount);
            m_vertexPoolResized = true;
            return offset;
        }

        void RenderWorld::deallocateVertexSpace(uint32_t offset, uint32_t vertexCount)
        {
            m_freeList.push_back(offset);
            m_freeList.push_back(vertexCount);
        }

        void RenderWorld::addEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
            PrimitiveType type, uint16_t materialIdx)
        {
            //SY_TRACEF("RenderWorld::addEntity: id=%llu, verts=%u, type=%u",
            //    id, vertexCount, static_cast<uint32_t>(type));

            EntityEntry entry;
            entry.entityId = id;
            entry.vertexCount = vertexCount;
            entry.allocatedSize = vertexCount;
            entry.primitiveType = static_cast<uint16_t>(type);
            entry.materialIndex = materialIdx;
            entry.flags = 0;
            entry.dirty = true;
            computeBBox(vertices, vertexCount, &entry.bbox[0], &entry.bbox[1],
                &entry.bbox[2], &entry.bbox[3]);

            if (m_entities.size() < 3)
            {
                SY_DEBUGF("RenderWorld::addEntity[%llu]: type=%u, verts=%u, bbox=[%.2f,%.2f]-[%.2f,%.2f], firstVert=(%.2f,%.2f,%.2f)(%.2f,%.2f,%.2f)",
                    id, static_cast<uint32_t>(type), vertexCount,
                    entry.bbox[0], entry.bbox[1], entry.bbox[2], entry.bbox[3],
                    vertices[0].px, vertices[0].py, vertices[0].pz,
                    vertexCount > 1 ? vertices[1].px : 0.f, vertexCount > 1 ? vertices[1].py : 0.f, vertexCount > 1 ? vertices[1].pz : 0.f);
            }

            entry.vertexOffset = allocateVertexSpace(vertexCount);
            //SY_TRACEF("RenderWorld::addEntity: vertex offset=%u", entry.vertexOffset);

            uint64_t key = m_entities.insert(entry);
            m_entityKeyMap[id] = key;

            std::copy(vertices, vertices + vertexCount, m_vertexPool.begin() + entry.vertexOffset);

            uint32_t denseIdx = static_cast<uint32_t>(m_entities.size() - 1);
            m_dirtyList.push_back(denseIdx);
            m_changeCount++;
            // 新增实体必须标记四叉树脏，否则 queryVisible 不会包含它
            m_quadTreeDirty = true;

            //SY_INFOF("RenderWorld::addEntity: total=%u, vertexPool=%u",
            //    (uint32_t)m_entities.size(), (uint32_t)m_vertexPool.size());
        }

        void RenderWorld::modifyEntity(EntityId id, const VertexP3C3* vertices, uint32_t vertexCount,
            uint16_t materialIdx)
        {
            auto it = m_entityKeyMap.find(id);
            if (it == m_entityKeyMap.end()) return;

            EntityEntry* entry = m_entities.find(it->second);
            if (!entry) return;

            entry->entityId = id;

            if (vertexCount > entry->allocatedSize)
            {
                deallocateVertexSpace(entry->vertexOffset, entry->allocatedSize);
                entry->vertexOffset = allocateVertexSpace(vertexCount);
                entry->allocatedSize = vertexCount;
            }

            entry->vertexCount = vertexCount;
            entry->materialIndex = materialIdx;
            entry->dirty = true;
            computeBBox(vertices, vertexCount, &entry->bbox[0], &entry->bbox[1],
                &entry->bbox[2], &entry->bbox[3]);

            std::copy(vertices, vertices + vertexCount, m_vertexPool.begin() + entry->vertexOffset);

            uint32_t denseIdx = static_cast<uint32_t>(entry - m_entities.begin());
            m_dirtyList.push_back(denseIdx);
            m_changeCount++;
            // 图元 bbox 可能变化，标记四叉树脏以确保下次查询重建
            m_quadTreeDirty = true;
        }

        void RenderWorld::removeEntity(EntityId id)
        {
            auto it = m_entityKeyMap.find(id);
            if (it == m_entityKeyMap.end()) return;

            EntityEntry* entry = m_entities.find(it->second);
            if (entry)
            {
                deallocateVertexSpace(entry->vertexOffset, entry->allocatedSize);
            }

            m_entities.erase(it->second);
            m_entityKeyMap.erase(it);
            m_changeCount++;

            m_quadTreeDirty = true;

            for (auto dirtyIt = m_dirtyList.begin(); dirtyIt != m_dirtyList.end(); )
            {
                if (*dirtyIt >= m_entities.size())
                {
                    dirtyIt = m_dirtyList.erase(dirtyIt);
                }
                else
                {
                    ++dirtyIt;
                }
            }
        }

        void RenderWorld::setEntityVisibility(EntityId id, bool visible)
        {
            auto it = m_entityKeyMap.find(id);
            if (it == m_entityKeyMap.end()) return;

            EntityEntry* entry = m_entities.find(it->second);
            if (!entry) return;

            if (visible)
                entry->flags &= ~kEntityFlagHidden;
            else
                entry->flags |= kEntityFlagHidden;
        }

        void RenderWorld::clearAllEntities()
        {
            m_entities.clear();
            m_entityKeyMap.clear();
            m_vertexPool.clear();
            m_freeList.clear();
            m_dirtyList.clear();
            m_quadTree.clear();
            m_quadTreeEntities.clear();
            m_quadTreeEntityNext.clear();
            m_visibleResult.clear();
            m_quadTreeDirty = false;
            m_vertexPoolResized = false;
            m_changeCount = 0;
            std::memset(m_lastViewMatrix, 0, sizeof(m_lastViewMatrix));
            m_generation++;
        }

        uint16_t RenderWorld::addMaterial(const MaterialDesc* desc)
        {
            uint16_t idx = static_cast<uint16_t>(m_materials.size());
            MaterialEntry me;
            me.desc = *desc;
            m_materials.push_back(me);
            return idx;
        }

        void RenderWorld::updateMaterial(uint16_t idx, const MaterialDesc* desc)
        {
            if (idx < static_cast<uint16_t>(m_materials.size()))
                m_materials[idx].desc = *desc;
        }

        void RenderWorld::queryVisible(const float viewMatrix[9], float viewWidth, float viewHeight,
            uint32_t* outIndices, uint32_t* outCount, uint32_t maxOut) const
        {
            (void)viewWidth;
            (void)viewHeight;

            // viewWidth/viewHeight 当前不直接参与视锥计算，因为 viewMatrix
            // 已是正交投影矩阵（包含缩放）。保留参数用于未来扩展和退化矩阵兜底。
            RenderWorld* self = const_cast<RenderWorld*>(this);

            if (outCount)
                *outCount = 0;

            // 四叉树重建条件：脏标记 / 变更累积超阈值 / 视图变化
            bool needsRebuild = m_quadTreeDirty || m_changeCount >= kRebuildThreshold;
            if (!needsRebuild)
            {
                needsRebuild = hasViewChanged(viewMatrix);
            }

            if (needsRebuild || m_quadTree.empty())
            {
                self->rebuildQuadTree();
                self->m_changeCount = 0;
                std::memcpy(self->m_lastViewMatrix, viewMatrix, sizeof(float) * 9);
            }

            m_visibleResult.clear();

            if (m_quadTree.empty() || m_entities.size() == 0)
            {
                if (outIndices) *outIndices = 0;
                if (outCount) *outCount = 0;
                return;
            }

            float m00 = viewMatrix[0], m01 = viewMatrix[3], m02 = viewMatrix[6];
            float m10 = viewMatrix[1], m11 = viewMatrix[4], m12 = viewMatrix[7];
            float m20 = viewMatrix[2], m21 = viewMatrix[5], m22 = viewMatrix[8];

            float det = m00 * (m11 * m22 - m12 * m21)
                - m01 * (m10 * m22 - m12 * m20)
                + m02 * (m10 * m21 - m11 * m20);

            float frustum[4];

            if (std::abs(det) < 1e-10f)
            {
                frustum[0] = -FLT_MAX;
                frustum[1] = -FLT_MAX;
                frustum[2] = FLT_MAX;
                frustum[3] = FLT_MAX;
            }
            else
            {
                float invDet = 1.0f / det;
                float inv[9];
                inv[0] = (m11 * m22 - m12 * m21) * invDet;
                inv[3] = (m02 * m21 - m01 * m22) * invDet;
                inv[6] = (m01 * m12 - m02 * m11) * invDet;
                inv[1] = (m12 * m20 - m10 * m22) * invDet;
                inv[4] = (m00 * m22 - m02 * m20) * invDet;
                inv[7] = (m02 * m10 - m00 * m12) * invDet;
                inv[2] = (m10 * m21 - m11 * m20) * invDet;
                inv[5] = (m01 * m20 - m00 * m21) * invDet;
                inv[8] = (m00 * m11 - m01 * m10) * invDet;

                static const float kCorners[4][2] = {
                    {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}
                };

                frustum[0] = FLT_MAX;  frustum[1] = FLT_MAX;
                frustum[2] = -FLT_MAX; frustum[3] = -FLT_MAX;

                for (int i = 0; i < 4; i++)
                {
                    float cx = kCorners[i][0], cy = kCorners[i][1];
                    float wx = inv[0] * cx + inv[3] * cy + inv[6];
                    float wy = inv[1] * cx + inv[4] * cy + inv[7];
                    float w = inv[2] * cx + inv[5] * cy + inv[8];
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

            while (!stack.empty())
            {
                uint32_t nodeIdx = stack.back();
                stack.pop_back();

                if (nodeIdx >= m_quadTree.size())
                    continue;

                const QuadTreeNode& node = m_quadTree[nodeIdx];

                if (!bboxIntersects(node.minX, node.minY, node.maxX, node.maxY,
                    frustum[0], frustum[1], frustum[2], frustum[3]))
                    continue;

                if (node.isLeaf)
                {
                    uint32_t iter = node.firstEntity;
                    while (iter != UINT32_MAX)
                    {
                        if (iter >= m_quadTreeEntities.size() || iter >= m_quadTreeEntityNext.size())
                            break;

                        uint32_t denseIdx = m_quadTreeEntities[iter];
                        if (denseIdx < m_entities.size() && isEntityVisible(denseIdx, frustum))
                        {
                            const EntityEntry& e = *(m_entities.begin() + denseIdx);
                            if (!(e.flags & kEntityFlagHidden))
                                m_visibleResult.push_back(denseIdx);
                        }
                        iter = m_quadTreeEntityNext[iter];
                    }
                }
                else
                {
                    for (uint32_t c = 0; c < 4; c++)
                    {
                        uint32_t childIdx = node.firstChild + c;
                        if (childIdx < m_quadTree.size())
                            stack.push_back(childIdx);
                    }
                }
            }

            uint32_t count = static_cast<uint32_t>(
                std::min(m_visibleResult.size(), static_cast<size_t>(maxOut)));
            if (outIndices && count > 0)
                std::memcpy(outIndices, m_visibleResult.data(), count * sizeof(uint32_t));
            if (outCount) *outCount = count;
        }

        void RenderWorld::update()
        {
            if (m_dirtyList.empty() && !m_vertexPoolResized)
                return;

            for (uint32_t denseIdx : m_dirtyList)
            {
                if (denseIdx >= m_entities.size()) continue;
                EntityEntry* entry = m_entities.begin() + denseIdx;
                if (entry->dirty)
                {
                    entry->dirty = false;
                }
            }

            m_dirtyList.clear();
            m_vertexPoolResized = false;
        }

        uint32_t RenderWorld::getDirtyVertexRanges(VertexUploadRange* outRanges, uint32_t maxRanges) const
        {
            if (!outRanges || maxRanges == 0)
                return 0;

            uint32_t count = 0;
            for (uint32_t denseIdx : m_dirtyList)
            {
                if (denseIdx >= m_entities.size())
                    continue;

                const EntityEntry& entry = *(m_entities.begin() + denseIdx);
                if (!entry.dirty)
                    continue;

                if (count < maxRanges)
                {
                    outRanges[count].vertexOffset = entry.vertexOffset;
                    outRanges[count].vertexCount = entry.vertexCount;
                    ++count;
                }
            }

            // 如果顶点池整体 resize，需要上传全部
            if (m_vertexPoolResized && count < maxRanges)
            {
                outRanges[count].vertexOffset = 0;
                outRanges[count].vertexCount = static_cast<uint32_t>(m_vertexPool.size());
                ++count;
            }

            return count;
        }

        void RenderWorld::clearDirtyFlags()
        {
            for (uint32_t denseIdx : m_dirtyList)
            {
                if (denseIdx >= m_entities.size())
                    continue;
                EntityEntry* entry = m_entities.begin() + denseIdx;
                entry->dirty = false;
            }
            m_dirtyList.clear();
            m_vertexPoolResized = false;
        }

        void RenderWorld::computeBBox(const VertexP3C3* verts, uint32_t count,
            float* outMinX, float* outMinY,
            float* outMaxX, float* outMaxY) const
        {
            if (count == 0)
            {
                *outMinX = *outMinY = *outMaxX = *outMaxY = 0.0f;
                return;
            }

            *outMinX = verts[0].px; *outMinY = verts[0].py;
            *outMaxX = verts[0].px; *outMaxY = verts[0].py;

            for (uint32_t i = 1; i < count; i++)
            {
                *outMinX = std::min(*outMinX, verts[i].px);
                *outMinY = std::min(*outMinY, verts[i].py);
                *outMaxX = std::max(*outMaxX, verts[i].px);
                *outMaxY = std::max(*outMaxY, verts[i].py);
            }
        }

        void RenderWorld::rebuildQuadTree()
        {
            m_quadTree.clear();
            m_quadTreeEntities.clear();
            m_quadTreeEntityNext.clear();

            if (m_entities.size() == 0)
            {
                m_quadTreeDirty = false;
                return;
            }

            float sceneMinX = FLT_MAX, sceneMinY = FLT_MAX;
            float sceneMaxX = -FLT_MAX, sceneMaxY = -FLT_MAX;

            for (const auto& entry : m_entities)
            {
                if (!std::isfinite(entry.bbox[0]) || !std::isfinite(entry.bbox[1]) ||
                    !std::isfinite(entry.bbox[2]) || !std::isfinite(entry.bbox[3]))
                {
                    continue;
                }

                if (entry.bbox[0] > entry.bbox[2] || entry.bbox[1] > entry.bbox[3])
                    continue;

                sceneMinX = std::min(sceneMinX, entry.bbox[0]);
                sceneMinY = std::min(sceneMinY, entry.bbox[1]);
                sceneMaxX = std::max(sceneMaxX, entry.bbox[2]);
                sceneMaxY = std::max(sceneMaxY, entry.bbox[3]);
            }

            if (!(sceneMinX <= sceneMaxX) || !(sceneMinY <= sceneMaxY))
            {
                m_quadTreeDirty = false;
                return;
            }

            float pad = std::max(sceneMaxX - sceneMinX, sceneMaxY - sceneMinY) * 0.01f;
            if (pad < 1.0f) pad = 1.0f;
            sceneMinX -= pad; sceneMinY -= pad;
            sceneMaxX += pad; sceneMaxY += pad;

            m_quadTree.push_back({ sceneMinX, sceneMinY, sceneMaxX, sceneMaxY,
                                  UINT32_MAX, UINT32_MAX, 0, true });

            for (uint32_t idx = 0; idx < m_entities.size(); ++idx)
            {
                insertQuadTree(0, idx, 0);
            }

            m_quadTreeDirty = false;
        }

        void RenderWorld::insertQuadTree(uint32_t nodeIdx, uint32_t entityDenseIdx, uint32_t depth)
        {
            if (nodeIdx >= m_quadTree.size() || entityDenseIdx >= m_entities.size())
                return;

            QuadTreeNode& node = m_quadTree[nodeIdx];
            const EntityEntry& entry = *(m_entities.begin() + entityDenseIdx);

            if (!std::isfinite(entry.bbox[0]) || !std::isfinite(entry.bbox[1]) ||
                !std::isfinite(entry.bbox[2]) || !std::isfinite(entry.bbox[3]))
                return;

            if (entry.bbox[0] > entry.bbox[2] || entry.bbox[1] > entry.bbox[3])
                return;

            if (!bboxIntersects(node.minX, node.minY, node.maxX, node.maxY,
                entry.bbox[0], entry.bbox[1], entry.bbox[2], entry.bbox[3]))
                return;

            if (node.isLeaf)
            {
                if (node.entityCount < kQuadTreeMaxEntities || depth >= kQuadTreeMaxDepth)
                {
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
                m_quadTree.push_back({ node.minX, node.minY, midX, midY,
                                      UINT32_MAX, UINT32_MAX, 0, true });
                m_quadTree.push_back({ midX, node.minY, node.maxX, midY,
                                      UINT32_MAX, UINT32_MAX, 0, true });
                m_quadTree.push_back({ midX, midY, node.maxX, node.maxY,
                                      UINT32_MAX, UINT32_MAX, 0, true });
                m_quadTree.push_back({ node.minX, midY, midX, node.maxY,
                                      UINT32_MAX, UINT32_MAX, 0, true });

                std::vector<uint32_t> existing;
                uint32_t iter = node.firstEntity;
                while (iter != UINT32_MAX)
                {
                    if (iter >= m_quadTreeEntities.size() || iter >= m_quadTreeEntityNext.size())
                        break;
                    existing.push_back(m_quadTreeEntities[iter]);
                    iter = m_quadTreeEntityNext[iter];
                }

                node.firstChild = firstChild;
                node.isLeaf = false;
                node.firstEntity = UINT32_MAX;
                node.entityCount = 0;

                for (uint32_t eidx : existing)
                {
                    insertQuadTree(firstChild, eidx, depth + 1);
                    insertQuadTree(firstChild + 1, eidx, depth + 1);
                    insertQuadTree(firstChild + 2, eidx, depth + 1);
                    insertQuadTree(firstChild + 3, eidx, depth + 1);
                }

                insertQuadTree(firstChild, entityDenseIdx, depth + 1);
                insertQuadTree(firstChild + 1, entityDenseIdx, depth + 1);
                insertQuadTree(firstChild + 2, entityDenseIdx, depth + 1);
                insertQuadTree(firstChild + 3, entityDenseIdx, depth + 1);
                return;
            }

            if (node.firstChild == UINT32_MAX)
                return;

            for (uint32_t c = 0; c < 4; c++)
            {
                uint32_t childIdx = node.firstChild + c;
                if (childIdx < m_quadTree.size())
                    insertQuadTree(childIdx, entityDenseIdx, depth + 1);
            }
        }

        bool RenderWorld::isEntityVisible(uint32_t denseIdx, const float frustum[4]) const
        {
            const EntityEntry& entry = *(m_entities.begin() + denseIdx);
            return bboxIntersects(entry.bbox[0], entry.bbox[1], entry.bbox[2], entry.bbox[3],
                frustum[0], frustum[1], frustum[2], frustum[3]);
        }

        const RenderWorld::EntityEntry* RenderWorld::getEntityEntries() const
        {
            return m_entities.dense_data();
        }

        uint32_t RenderWorld::getEntityCount() const
        {
            return m_entities.size();
        }

        bool RenderWorld::hasViewChanged(const float viewMatrix[9]) const
        {
            for (int i = 0; i < 9; i++)
            {
                if (std::abs(viewMatrix[i] - m_lastViewMatrix[i]) > kViewChangeEpsilon)
                {
                    return true;
                }
            }
            return false;
        }
    } // namespace core
} // namespace render