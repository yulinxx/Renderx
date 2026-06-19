#include "RenderDataConsumer.h"
#include "ShaderManager.h"
#include <QOpenGLShaderProgram>
#include <cstring>

RenderDataConsumer::RenderDataConsumer()
{
    m_defaultColor[0] = 0.0f;
    m_defaultColor[1] = 0.0f;
    m_defaultColor[2] = 0.0f;
    m_defaultColor[3] = 1.0f;
}

RenderDataConsumer::~RenderDataConsumer()
{
}

bool RenderDataConsumer::initialize()
{
    if (m_bInitialized)
        return true;

    if (!initializeOpenGLFunctions())
        return false;

    // 创建 VAO/VBO
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);

    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);

    // 布局 0: position (vec2)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Render::RenderVertex), nullptr);

    // 布局 1: vertexColor (vec3)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Render::RenderVertex),
        reinterpret_cast<void*>(offsetof(Render::RenderVertex, color)));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    m_bInitialized = true;
    return true;
}

void RenderDataConsumer::cleanup()
{
    if (!m_bInitialized)
        return;

    if (m_vao)
    {
        glDeleteVertexArrays(1, &m_vao);
        m_vao = 0;
    }

    if (m_vbo)
    {
        glDeleteBuffers(1, &m_vbo);
        m_vbo = 0;
    }

    m_vboCapacity = 0;
    m_bInitialized = false;
}

void RenderDataConsumer::setViewMatrix(const Ut::Mat3f& matrix)
{
    m_viewMatrix = matrix;
}

void RenderDataConsumer::setDefaultColor(float r, float g, float b, float a)
{
    m_defaultColor[0] = r;
    m_defaultColor[1] = g;
    m_defaultColor[2] = b;
    m_defaultColor[3] = a;
}

void RenderDataConsumer::setDefaultLineWidth(float width)
{
    m_defaultLineWidth = width;
}

void RenderDataConsumer::ensureVBOCapacity(size_t vertexCount)
{
    size_t requiredBytes = vertexCount * sizeof(Render::RenderVertex);
    if (requiredBytes <= m_vboCapacity)
        return;

    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    if (m_vboCapacity == 0)
    {
        // 首次分配
        glBufferData(GL_ARRAY_BUFFER, requiredBytes, nullptr, GL_DYNAMIC_DRAW);
    }
    else
    {
        // 扩容：先 orphan 旧缓冲，再分配新大小
        glBufferData(GL_ARRAY_BUFFER, requiredBytes, nullptr, GL_DYNAMIC_DRAW);
    }
    m_vboCapacity = requiredBytes;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// =====================================================================
// 新管线：RenderCommandList（批处理优化版）
// 将同类型命令合并为批量上传+绘制，大幅减少 Draw Call
// =====================================================================

void RenderDataConsumer::render(const Render::RenderCommandList& commands)
{
    if (!m_bInitialized || commands.empty())
        return;

    QOpenGLShaderProgram* shader = ShaderManager::instance().sceneShader();
    if (!shader)
        return;

    shader->bind();

    // 设置视图矩阵
    GLint projLoc = ShaderManager::instance().uniformLocation("scene", "projectionView");
    glUniformMatrix3fv(projLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);

    GLint colorLoc = ShaderManager::instance().uniformLocation("scene", "uniformColor");
    GLint useVtxLoc = ShaderManager::instance().uniformLocation("scene", "useVertexColor");

    glBindVertexArray(m_vao);

    // 批处理：将连续同类型命令合并
    // 可合并条件：相同 primitiveType、相同 useVertexColors、相同 lineWidth
    size_t i = 0;
    while (i < commands.size())
    {
        const auto& cmd = commands.commands[i];
        if (cmd.isEmpty())
        {
            ++i;
            continue;
        }

        // 收集可合并到同一批次的命令
        std::vector<const Render::RenderCommand*> batch;
        batch.push_back(&cmd);

        size_t j = i + 1;
        while (j < commands.size())
        {
            const auto& next = commands.commands[j];
            if (!next.isEmpty() && canBatch(cmd, next))
            {
                batch.push_back(&next);
                ++j;
            }
            else
            {
                break;
            }
        }

        // 渲染这一批
        if (batch.size() == 1)
        {
            renderCommand(*batch[0]);
        }
        else
        {
            renderBatch(batch);
        }

        i = j;
    }

    glBindVertexArray(0);
    shader->release();
}

bool RenderDataConsumer::canBatch(const Render::RenderCommand& a, const Render::RenderCommand& b)
{
    // LineStrip、LineLoop 和 TriangleFan 不能合并
    // TriangleFan 的每个命令是独立的扇形（如点图元），合并会导致多个扇形连成一片
    if (a.primitiveType == Render::RenderPrimitiveType::LineStrip ||
        a.primitiveType == Render::RenderPrimitiveType::LineLoop ||
        a.primitiveType == Render::RenderPrimitiveType::TriangleFan)
        return false;

    return a.primitiveType == b.primitiveType
        && a.useVertexColors == b.useVertexColors
        && std::abs(a.lineWidth - b.lineWidth) < 0.001f;
}

void RenderDataConsumer::renderBatch(const std::vector<const Render::RenderCommand*>& batch)
{
    if (batch.empty())
        return;

    QOpenGLShaderProgram* shader = ShaderManager::instance().sceneShader();
    if (!shader)
        return;

    GLint colorLoc = ShaderManager::instance().uniformLocation("scene", "uniformColor");
    GLint useVtxLoc = ShaderManager::instance().uniformLocation("scene", "useVertexColor");

    // 计算总顶点数
    size_t totalVertices = 0;
    for (const auto* cmd : batch)
    {
        totalVertices += cmd->vertexCount();
    }

    if (totalVertices == 0)
        return;

    const auto& first = *batch[0];

    // 设置 uniform
    if (first.useVertexColors && first.vertexCount() > 0)
    {
        const auto& c = first.vertices[0].color;
        glUniform4f(colorLoc, c.x(), c.y(), c.z(), 1.0f);
        glUniform1i(useVtxLoc, GL_TRUE);
    }
    else
    {
        glUniform4f(colorLoc, m_defaultColor[0], m_defaultColor[1], m_defaultColor[2], m_defaultColor[3]);
        glUniform1i(useVtxLoc, GL_FALSE);
    }

    glLineWidth(first.lineWidth > 0.0f ? first.lineWidth : m_defaultLineWidth);

    // 确保 VBO 容量足够
    ensureVBOCapacity(totalVertices);

    // 合并所有顶点到 VBO
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    size_t offset = 0;
    for (const auto* cmd : batch)
    {
        size_t bytes = cmd->vertexCount() * sizeof(Render::RenderVertex);
        glBufferSubData(GL_ARRAY_BUFFER, offset, bytes, cmd->vertices.data());
        offset += bytes;
    }

    // 一次绘制调用
    GLenum mode = toGLPrimitiveType(first.primitiveType);
    glDrawArrays(mode, 0, static_cast<GLsizei>(totalVertices));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void RenderDataConsumer::renderCommand(const Render::RenderCommand& cmd)
{
    QOpenGLShaderProgram* shader = ShaderManager::instance().sceneShader();
    if (!shader)
        return;

    // 设置 uniform color（当不使用顶点颜色时生效）
    GLint colorLoc = ShaderManager::instance().uniformLocation("scene", "uniformColor");
    GLint useVtxLoc = ShaderManager::instance().uniformLocation("scene", "useVertexColor");

    if (cmd.useVertexColors && cmd.vertexCount() > 0)
    {
        // 使用第一个顶点的颜色作为 uniform fallback（实际由顶点属性决定）
        const auto& c = cmd.vertices[0].color;
        glUniform4f(colorLoc, c.x(), c.y(), c.z(), 1.0f);
        glUniform1i(useVtxLoc, GL_TRUE);
    }
    else
    {
        glUniform4f(colorLoc, m_defaultColor[0], m_defaultColor[1], m_defaultColor[2], m_defaultColor[3]);
        glUniform1i(useVtxLoc, GL_FALSE);
    }

    // 线宽
    glLineWidth(cmd.lineWidth > 0.0f ? cmd.lineWidth : m_defaultLineWidth);

    // 上传顶点数据
    ensureVBOCapacity(cmd.vertexCount());
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
        cmd.vertexCount() * sizeof(Render::RenderVertex),
        cmd.vertices.data());

    // 绘制
    GLenum mode = toGLPrimitiveType(cmd.primitiveType);
    glDrawArrays(mode, 0, static_cast<GLsizei>(cmd.vertexCount()));

    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

GLenum RenderDataConsumer::toGLPrimitiveType(Render::RenderPrimitiveType type)
{
    switch (type)
    {
        case Render::RenderPrimitiveType::Points:       return GL_POINTS;
        case Render::RenderPrimitiveType::Lines:        return GL_LINES;
        case Render::RenderPrimitiveType::LineStrip:    return GL_LINE_STRIP;
        case Render::RenderPrimitiveType::LineLoop:     return GL_LINE_LOOP;
        case Render::RenderPrimitiveType::Triangles:    return GL_TRIANGLES;
        case Render::RenderPrimitiveType::TriangleFan:  return GL_TRIANGLE_FAN;
        default:                                    return GL_LINE_STRIP;
    }
}
