#include "RenderCore/RenderWorld.h"
#include "RenderCore/GL46Backend.h"

#include <algorithm>
#include <numeric>

namespace RenderCore
{

// ==================== RenderWorld 实现 ========== =========

RenderWorld::RenderWorld()
{
}

RenderWorld::~RenderWorld()
{
    shutdown();
}

bool RenderWorld::initialize(EBackendType backendType)
{
    m_backend = createBackend(backendType);
    if (!m_backend)
        return false;

    return m_backend->initialize();
}

void RenderWorld::shutdown()
{
    if (m_backend)
    {
        m_backend->shutdown();
        m_backend.reset();
    }

    m_entities.clear();
    m_dirtyEntities.clear();
    m_pendingDelete.clear();
    m_updateCommands.clear();
}

void RenderWorld::update()
{
    // 调用外部更新回调（用于同步Engine数据）
    if (m_updateCallback)
    {
        m_updateCallback();
    }

    // 处理脏实体
    processDirtyEntities();

    // 提交更新到后端
    commitUpdates();

    m_frameCount++;
}

void RenderWorld::setEntity(EntityId id,
                             std::vector<Vertex> vertices,
                             EPrimitiveType primitiveType,
                             float lineWidth)
{
    auto it = m_entities.find(id);
    if (it == m_entities.end())
    {
        // 新增
        EntityEntry entry;
        entry.id = id;
        entry.generation = 1;
        entry.vertices = std::move(vertices);
        entry.primitiveType = primitiveType;
        entry.lineWidth = lineWidth;
        entry.dirtyFlags = EDirtyFlag::All;

        // 计算包围盒
        if (!entry.vertices.empty())
        {
            float minX = entry.vertices[0].position.x();
            float maxX = minX;
            float minY = entry.vertices[0].position.y();
            float maxY = minY;
            float minZ = 0.0f, maxZ = 0.0f;

            for (const auto& v : entry.vertices)
            {
                minX = std::min(minX, v.position.x());
                maxX = std::max(maxX, v.position.x());
                minY = std::min(minY, v.position.y());
                maxY = std::max(maxY, v.position.y());
            }
            entry.boundingBox = Render::BBox3f(Render::Vec3f(minX, minY, minZ),
                                            Render::Vec3f(maxX, maxY, maxZ));
        }

        m_entities[id] = std::move(entry);
        m_dirtyEntities.push_back(id);
    }
    else
    {
        // 更新
        EntityEntry& entry = it->second;
        entry.generation++;
        entry.vertices = std::move(vertices);
        entry.primitiveType = primitiveType;
        entry.lineWidth = lineWidth;
        entry.dirtyFlags = EDirtyFlag::All;

        // 重新计算包围盒
        if (!entry.vertices.empty())
        {
            float minX = entry.vertices[0].position.x();
            float maxX = minX;
            float minY = entry.vertices[0].position.y();
            float maxY = minY;

            for (const auto& v : entry.vertices)
            {
                minX = std::min(minX, v.position.x());
                maxX = std::max(maxX, v.position.x());
                minY = std::min(minY, v.position.y());
                maxY = std::max(maxY, v.position.y());
            }
            entry.boundingBox = Render::BBox3f(Render::Vec3f(minX, minY, 0.0f),
                                            Render::Vec3f(maxX, maxY, 0.0f));
        }

        markDirty(id, EDirtyFlag::All);
    }
}

void RenderWorld::setEntities(std::span<const EntityId> ids,
                               std::span<const std::vector<Vertex>> vertices,
                               std::span<const EPrimitiveType> primitiveTypes,
                               std::span<const float> lineWidths)
{
    size_t count = ids.size();
    if (count == 0 || vertices.size() != count ||
        primitiveTypes.size() != count || lineWidths.size() != count)
    {
        return;
    }

    for (size_t i = 0; i < count; ++i)
    {
        setEntity(ids[i], std::vector<Vertex>(vertices[i]),
                  primitiveTypes[i], lineWidths[i]);
    }
}

void RenderWorld::removeEntity(EntityId id)
{
    auto it = m_entities.find(id);
    if (it != m_entities.end())
    {
        // 软删除
        m_pendingDelete.insert(id);
        m_dirtyEntities.push_back(id);

        // 从活跃实体中移除
        m_entities.erase(it);
    }
}

void RenderWorld::removeEntities(std::span<const EntityId> ids)
{
    for (EntityId id : ids)
    {
        removeEntity(id);
    }
}

void RenderWorld::clear()
{
    // 全部标记为待删除
    for (const auto& pair : m_entities)
    {
        m_pendingDelete.insert(pair.first);
    }
    m_entities.clear();
    m_dirtyEntities.clear();
}

void RenderWorld::render(const RenderState& state)
{
    if (!m_backend)
        return;

    m_backend->beginFrame();
    m_backend->setRenderState(state);
    m_backend->renderAll();
    m_backend->endFrame();
}

bool RenderWorld::hasEntity(EntityId id) const
{
    return m_entities.find(id) != m_entities.end();
}

const EntityEntry* RenderWorld::getEntity(EntityId id) const
{
    auto it = m_entities.find(id);
    return it != m_entities.end() ? &it->second : nullptr;
}

size_t RenderWorld::getDirtyCount() const
{
    return m_dirtyEntities.size();
}

void RenderWorld::markDirty(EntityId id, EDirtyFlag flags)
{
    auto it = std::find(m_dirtyEntities.begin(), m_dirtyEntities.end(), id);
    if (it == m_dirtyEntities.end())
    {
        m_dirtyEntities.push_back(id);
    }
}

void RenderWorld::processDirtyEntities()
{
    // 确保容量
    m_updateCommands.reserve(m_dirtyEntities.size());

    for (EntityId id : m_dirtyEntities)
    {
        // 检查是否待删除
        if (m_pendingDelete.find(id) != m_pendingDelete.end())
        {
            UpdateCommand cmd;
            cmd.op = EUpdateOp::Remove;
            cmd.entityId = id;
            m_updateCommands.push_back(cmd);
            continue;
        }

        // 活跃实体
        auto it = m_entities.find(id);
        if (it == m_entities.end())
            continue;

        const EntityEntry& entry = it->second;

        UpdateCommand cmd;
        cmd.op = EUpdateOp::Modify;
        cmd.entityId = id;
        cmd.generation = entry.generation;
        cmd.vertices = entry.vertices;
        cmd.primitiveType = entry.primitiveType;
        cmd.lineWidth = entry.lineWidth;
        m_updateCommands.push_back(cmd);
    }

    // 清除待删除集合
    m_pendingDelete.clear();

    // 清除脏标记
    m_dirtyEntities.clear();
}

void RenderWorld::commitUpdates()
{
    if (!m_backend || m_updateCommands.empty())
        return;

    m_backend->submitUpdates(m_updateCommands);
    m_updateCommands.clear();
}

} // namespace RenderCore
