#include "render_world_3d.h"

#include <algorithm>
#include <cstring>

namespace render
{
    struct RenderWorld3D::Impl
    {
        std::vector<EntityEntry3D> entities;
        std::vector<VertexP3N3> vertexPool;
        std::vector<uint32_t> indexPool;
        std::unordered_map<EntityId, uint32_t> entityIdToDense;
        bool initialized = false;
    };

    RenderWorld3D::~RenderWorld3D()
    {
        delete m_impl;
    }

    bool RenderWorld3D::initialize(uint32_t initialVertexCapacity,
        uint32_t initialIndexCapacity)
    {
        if (!m_impl)
            m_impl = new Impl();

        m_impl->vertexPool.reserve(initialVertexCapacity);
        m_impl->indexPool.reserve(initialIndexCapacity);
        m_impl->entities.reserve(1024);
        m_impl->initialized = true;
        return true;
    }

    void RenderWorld3D::shutdown()
    {
        if (m_impl)
        {
            delete m_impl;
            m_impl = nullptr;
        }
    }

    void RenderWorld3D::addEntity(EntityId id, const VertexP3N3* vertices,
        uint32_t vertexCount, const uint32_t* indices,
        uint32_t indexCount, uint16_t materialIdx)
    {
        if (!m_impl || !vertices || vertexCount == 0) return;

        EntityEntry3D entry;
        entry.entityId = id;
        entry.vertexOffset = static_cast<uint32_t>(m_impl->vertexPool.size());
        entry.vertexCount = vertexCount;
        entry.indexOffset = static_cast<uint32_t>(m_impl->indexPool.size());
        entry.indexCount = indexCount;
        entry.primitiveType = static_cast<uint16_t>(PrimitiveType::TriangleList);
        entry.materialIndex = materialIdx;
        entry.dirty = true;

        for (uint32_t i = 0; i < vertexCount; ++i)
        {
            m_impl->vertexPool.push_back(vertices[i]);
            if (i == 0)
            {
                entry.bboxMin[0] = entry.bboxMax[0] = vertices[i].px;
                entry.bboxMin[1] = entry.bboxMax[1] = vertices[i].py;
                entry.bboxMin[2] = entry.bboxMax[2] = vertices[i].pz;
            }
            else
            {
                entry.bboxMin[0] = std::min(entry.bboxMin[0], vertices[i].px);
                entry.bboxMin[1] = std::min(entry.bboxMin[1], vertices[i].py);
                entry.bboxMin[2] = std::min(entry.bboxMin[2], vertices[i].pz);
                entry.bboxMax[0] = std::max(entry.bboxMax[0], vertices[i].px);
                entry.bboxMax[1] = std::max(entry.bboxMax[1], vertices[i].py);
                entry.bboxMax[2] = std::max(entry.bboxMax[2], vertices[i].pz);
            }
        }

        for (uint32_t i = 0; i < indexCount; ++i)
            m_impl->indexPool.push_back(indices[i]);

        uint32_t denseIdx = static_cast<uint32_t>(m_impl->entities.size());
        m_impl->entities.push_back(entry);
        m_impl->entityIdToDense[id] = denseIdx;
    }

    void RenderWorld3D::removeEntity(EntityId id)
    {
        if (!m_impl) return;
        auto it = m_impl->entityIdToDense.find(id);
        if (it == m_impl->entityIdToDense.end()) return;

        uint32_t denseIdx = it->second;
        if (denseIdx < m_impl->entities.size())
            m_impl->entities[denseIdx].entityId = 0;
        m_impl->entityIdToDense.erase(it);
    }

    void RenderWorld3D::clear()
    {
        if (!m_impl) return;
        m_impl->entities.clear();
        m_impl->vertexPool.clear();
        m_impl->indexPool.clear();
        m_impl->entityIdToDense.clear();
    }

    const EntityEntry3D* RenderWorld3D::getEntityEntries() const
    {
        return m_impl ? m_impl->entities.data() : nullptr;
    }

    uint32_t RenderWorld3D::getEntityCount() const
    {
        return m_impl ? static_cast<uint32_t>(m_impl->entities.size()) : 0;
    }

    const VertexP3N3* RenderWorld3D::getVertexData() const
    {
        return m_impl ? m_impl->vertexPool.data() : nullptr;
    }

    uint32_t RenderWorld3D::getTotalVertexCount() const
    {
        return m_impl ? static_cast<uint32_t>(m_impl->vertexPool.size()) : 0;
    }

    const uint32_t* RenderWorld3D::getIndexData() const
    {
        return m_impl ? m_impl->indexPool.data() : nullptr;
    }

    uint32_t RenderWorld3D::getTotalIndexCount() const
    {
        return m_impl ? static_cast<uint32_t>(m_impl->indexPool.size()) : 0;
    }

    bool RenderWorld3D::hasDirtyEntities() const
    {
        if (!m_impl) return false;
        for (const auto& e : m_impl->entities)
            if (e.dirty) return true;
        return false;
    }

    void RenderWorld3D::clearDirtyFlags()
    {
        if (!m_impl) return;
        for (auto& e : m_impl->entities)
            e.dirty = false;
    }
} // namespace render