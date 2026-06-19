#include "RenderCore/RenderBatch.h"
#include "RenderCore/RenderBuffer.h"

#include <GL/glew.h>
#include <algorithm>
#include <numeric>

namespace RenderCore
{

// ==================== BatchRenderer 实现 ========== =========

BatchRenderer::BatchRenderer()
{
}

BatchRenderer::~BatchRenderer()
{
    shutdown();
}

bool BatchRenderer::initialize()
{
    // 创建VAO
    glGenVertexArrays(1, &m_vao);
    if (m_vao == 0)
        return false;

    // 初始化顶点缓冲区
    if (!m_vertexBuffer.create(EBufferType::Vertex, EBufferUsage::Dynamic,
                               1024 * 1024 * sizeof(Vertex), nullptr))
        return false;

    // 初始化间接绘制缓冲区
    if (!m_indirectBuffer.create(EBufferType::Indirect, EBufferUsage::Dynamic,
                                 1024 * sizeof(DrawArraysIndirectCommand), nullptr))
        return false;

    glBindVertexArray(m_vao);

    // 设置顶点格式
    // location 0: position (vec3)
    // location 1: color (vec3)
    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribFormat(1, 3, GL_FLOAT, GL_FALSE, sizeof(Render::Vec3f));

    glVertexAttribBinding(0, 0);
    glVertexAttribBinding(1, 0);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    // 绑定顶点缓冲区
    glBindVertexBuffer(0, m_vertexBuffer.getHandle(), 0, sizeof(Vertex));

    glBindVertexArray(0);

    return true;
}

void BatchRenderer::shutdown()
{
    if (m_vao)
    {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    m_vertexBuffer.destroy();
    m_indirectBuffer.destroy();

    m_mergedVertices.clear();
    m_indirectCommands.clear();
    m_batchCommands.clear();
    m_batchCounts.clear();
}

void BatchRenderer::addToBatch(EntityId entityId,
                               std::span<const Vertex> vertices,
                               EPrimitiveType primitiveType,
                               float lineWidth)
{
    if (vertices.empty())
        return;

    BatchCommand cmd;
    cmd.entityId = entityId;
    cmd.primitiveType = primitiveType;
    cmd.firstVertex = m_mergedVertices.size();
    cmd.vertexCount = vertices.size();
    cmd.lineWidth = lineWidth;

    m_batchCommands.push_back(cmd);
    m_mergedVertices.insert(m_mergedVertices.end(), vertices.begin(), vertices.end());

    m_batchCounts[primitiveType]++;
    m_dirtyBatches = true;
}

void BatchRenderer::removeFromBatch(EntityId entityId)
{
    auto it = std::find_if(m_batchCommands.begin(), m_batchCommands.end(),
        [entityId](const BatchCommand& cmd) { return cmd.entityId == entityId; });

    if (it != m_batchCommands.end())
    {
        m_batchCounts[it->primitiveType]--;
        m_batchCommands.erase(it);
        m_dirtyBatches = true;
    }
}

void BatchRenderer::updateInBatch(EntityId entityId,
                                  std::span<const Vertex> vertices,
                                  float lineWidth)
{
    auto it = std::find_if(m_batchCommands.begin(), m_batchCommands.end(),
        [entityId](const BatchCommand& cmd) { return cmd.entityId == entityId; });

    if (it != m_batchCommands.end())
    {
        // 更新顶点数据
        if (vertices.size() == it->vertexCount)
        {
            std::copy(vertices.begin(), vertices.end(),
                      m_mergedVertices.begin() + it->firstVertex);
        }
        else
        {
            // 大小改变，需要重新合并
            m_dirtyBatches = true;
        }
        it->lineWidth = lineWidth;
    }
}

void BatchRenderer::clearAllBatches()
{
    m_batchCommands.clear();
    m_mergedVertices.clear();
    m_indirectCommands.clear();
    m_batchCounts.clear();
    m_dirtyBatches = true;
}

void BatchRenderer::render(const RenderState& state)
{
    if (m_mergedVertices.empty())
        return;

    // 合并批次
    if (m_dirtyBatches)
    {
        mergeBatches();
        m_dirtyBatches = false;
    }

    // 更新间接缓冲区
    if (m_dirtyIndirect)
    {
        updateIndirectBuffer();
        m_dirtyIndirect = false;
    }

    // 绑定VAO和程序
    glBindVertexArray(m_vao);

    // 设置线宽（注意：OpenGL线宽是状态设置，不是per-draw的）
    glLineWidth(state.lineWidth);

    // 使用多绘制间接
    for (size_t i = 0; i < m_indirectCommands.size(); ++i)
    {
        const auto& cmd = m_indirectCommands[i];
        GLenum mode = toGLPrimitiveType(m_batchCommands[i].primitiveType);
        glDrawArraysInstanced(mode,
                             cmd.firstVertex,
                             cmd.vertexCount,
                             cmd.instanceCount);
    }

    glBindVertexArray(0);
}

size_t BatchRenderer::getBatchCount(EPrimitiveType type) const
{
    auto it = m_batchCounts.find(type);
    return it != m_batchCounts.end() ? it->second : 0;
}

void BatchRenderer::mergeBatches()
{
    // 按图元类型分组
    std::sort(m_batchCommands.begin(), m_batchCommands.end(),
        [](const BatchCommand& a, const BatchCommand& b)
        {
            return a.primitiveType < b.primitiveType;
        });

    // 重新计算每条的firstVertex
    size_t currentOffset = 0;
    for (auto& cmd : m_batchCommands)
    {
        cmd.firstVertex = currentOffset;
        currentOffset += cmd.vertexCount;
    }

    m_indirectCommands.resize(m_batchCommands.size());
    for (size_t i = 0; i < m_batchCommands.size(); ++i)
    {
        m_indirectCommands[i].vertexCount = m_batchCommands[i].vertexCount;
        m_indirectCommands[i].instanceCount = 1;
        m_indirectCommands[i].firstVertex = m_batchCommands[i].firstVertex;
        m_indirectCommands[i].baseInstance = 0;
    }
}

void BatchRenderer::updateIndirectBuffer()
{
    if (m_indirectCommands.empty())
        return;

    m_indirectBuffer.replace(m_indirectCommands.size() * sizeof(DrawArraysIndirectCommand),
                               m_indirectCommands.data());

    // 绑定间接缓冲区用于渲染
    glBindVertexBuffer(1, m_indirectBuffer.getHandle(), 0, sizeof(DrawArraysIndirectCommand));
}

GLenum toGLPrimitiveType(EPrimitiveType type)
{
    switch (type)
    {
        case EPrimitiveType::Points:        return GL_POINTS;
        case EPrimitiveType::Lines:         return GL_LINES;
        case EPrimitiveType::LineStrip:     return GL_LINE_STRIP;
        case EPrimitiveType::LineLoop:      return GL_LINE_LOOP;
        case EPrimitiveType::Triangles:     return GL_TRIANGLES;
        case EPrimitiveType::TriangleStrip:  return GL_TRIANGLE_STRIP;
        case EPrimitiveType::TriangleFan:   return GL_TRIANGLE_FAN;
        default:                            return GL_LINES;
    }
}

} // namespace RenderCore
