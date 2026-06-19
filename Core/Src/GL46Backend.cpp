#include "RenderCore/GL46Backend.h"
#include "RenderCore/RenderBuffer.h"

#include <GL/glew.h>
#include <cstring>
#include <algorithm>
#include <numeric>

namespace RenderCore
{

// ==================== 顶点着色器源码 ====================

static const char* VERTEX_SHADER_SOURCE = R"(
#version 450 core

// 顶点属性
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;

// Uniform
layout(binding = 0) uniform ViewMatrix
{
    mat3 u_viewMatrix;
};

// 实例属性（用于未来扩展实例化渲染）
// layout(location = 2) in uint a_entityId;

// 输出到片段着色器
out vec3 v_color;

void main()
{
    // 应用视图矩阵（2D变换）
    vec2 pos = u_viewMatrix * vec3(a_position.xy, 1.0);
    gl_Position = vec4(pos, a_position.z, 1.0);
    v_color = a_color;
}
)";

// ==================== 片段着色器源码 ====================

static const char* FRAGMENT_SHADER_SOURCE = R"(
#version 450 core

in vec3 v_color;
out vec4 fragColor;

void main()
{
    fragColor = vec4(v_color, 1.0);
}
)";

// ==================== 辅助函数 ========== =========

static GLenum toGLPrimitive(EPrimitiveType type)
{
    switch (type)
    {
        case EPrimitiveType::Points:       return GL_POINTS;
        case EPrimitiveType::Lines:        return GL_LINES;
        case EPrimitiveType::LineStrip:    return GL_LINE_STRIP;
        case EPrimitiveType::LineLoop:     return GL_LINE_LOOP;
        case EPrimitiveType::Triangles:    return GL_TRIANGLES;
        case EPrimitiveType::TriangleStrip: return GL_TRIANGLE_STRIP;
        case EPrimitiveType::TriangleFan:  return GL_TRIANGLE_FAN;
        default:                           return GL_LINES;
    }
}

// ==================== GL46Backend 实现 ========== =========

GL46Backend::GL46Backend()
{
    m_vertexCache.reserve(INITIAL_VERTEX_CAPACITY);
}

GL46Backend::~GL46Backend()
{
    shutdown();
}

bool GL46Backend::initialize()
{
    if (m_initialized)
        return true;

    // 初始化OpenGL函数
    if (glewInit() != GLEW_OK)
        return false;

    // 编译着色器
    if (!compileShaders())
        return false;

    // 创建VAO
    if (!createVertexArray())
        return false;

    // 创建间接绘制缓冲区
    if (!createIndirectBuffer())
        return false;

    // 初始化顶点缓冲区（使用Buffer Storage）
    m_state.vertexBufferMemory = INITIAL_VERTEX_CAPACITY * sizeof(Vertex);
    if (!m_state.vertexBuffer.create(EBufferType::Vertex, EBufferUsage::Dynamic,
                                      m_state.vertexBufferMemory, nullptr))
        return false;

    // 初始化批次渲染器
    if (!m_batchRenderer.initialize())
        return false;

    m_initialized = true;
    return true;
}

void GL46Backend::shutdown()
{
    if (!m_initialized)
        return;

    m_batchRenderer.shutdown();

    if (m_state.vao)
    {
        glDeleteVertexArrays(1, &m_state.vao);
        m_state.vao = 0;
    }

    if (m_state.sceneProgram)
    {
        glDeleteProgram(m_state.sceneProgram);
        m_state.sceneProgram = 0;
    }

    m_state.vertexBuffer.destroy();
    m_state.indirectBuffer.destroy();

    m_vertexCache.clear();
    m_entityMap.clear();
    m_dirtyEntities.clear();
    m_pendingCommands.clear();

    m_entityCount = 0;
    m_initialized = false;
}

void GL46Backend::beginFrame()
{
    m_frameVertexCount = 0;
    m_pendingCommands.clear();
    m_dirtyEntities.clear();
}

void GL46Backend::endFrame()
{
    // 如果有脏数据，上传到GPU
    if (!m_dirtyEntities.empty())
    {
        // 合并顶点数据
        m_batchRenderer.clearAllBatches();

        // 按图元类型合并顶点
        std::vector<Vertex> allVertices;
        std::unordered_map<EPrimitiveType, std::vector<BatchCommand>, EnumHash> batches;

        size_t offset = 0;
        for (const auto& entityPair : m_entityMap)
        {
            EntityId entityId = entityPair.first;
            const EntityInfo& info = entityPair.second;

            auto it = std::find_if(m_pendingCommands.begin(), m_pendingCommands.end(),
                [entityId](const UpdateCommand& cmd) { return cmd.entityId == entityId; });

            if (it != m_pendingCommands.end())
            {
                const auto& cmd = *it;
                if (cmd.op != EUpdateOp::Remove)
                {
                    BatchCommand batch;
                    batch.entityId = entityId;
                    batch.primitiveType = cmd.primitiveType;
                    batch.firstVertex = allVertices.size();
                    batch.vertexCount = cmd.vertices.size();
                    batch.lineWidth = cmd.lineWidth;

                    batches[cmd.primitiveType].push_back(batch);
                    allVertices.insert(allVertices.end(), cmd.vertices.begin(), cmd.vertices.end());
                }
            }
            else
            {
                // 使用缓存数据
                // TODO: 需要缓存顶点数据
            }
        }

        // 上传合并后的顶点数据
        if (!allVertices.empty())
        {
            m_state.vertexBuffer.replace(allVertices.size() * sizeof(Vertex), allVertices.data());

            // 添加到批次渲染器
            for (const auto& batchPair : batches)
            {
                EPrimitiveType primType = batchPair.first;
                const auto& commands = batchPair.second;
                for (const auto& cmd : commands)
                {
                    std::span<const Vertex> verts(
                        allVertices.data() + cmd.firstVertex,
                        cmd.vertexCount);
                    m_batchRenderer.addToBatch(cmd.entityId, verts, cmd.primitiveType, cmd.lineWidth);
                }
            }
        }
    }
}

void GL46Backend::submitUpdates(std::span<const UpdateCommand> commands)
{
    m_pendingCommands.insert(m_pendingCommands.end(), commands.begin(), commands.end());

    for (const auto& cmd : commands)
    {
        switch (cmd.op)
        {
            case EUpdateOp::Add:
            case EUpdateOp::Modify:
            {
                auto it = m_entityMap.find(cmd.entityId);
                if (it == m_entityMap.end())
                {
                    // 新增
                    EntityInfo info;
                    info.generation = cmd.generation;
                    info.vertexCount = cmd.vertices.size();
                    info.bufferOffset = 0;  // 将在合并时计算
                    m_entityMap[cmd.entityId] = info;
                    m_entityCount++;
                }
                else
                {
                    // 修改
                    if (it->second.generation != cmd.generation)
                    {
                        it->second.generation = cmd.generation;
                        it->second.vertexCount = cmd.vertices.size();
                        m_dirtyEntities.push_back(cmd.entityId);
                    }
                }
                break;
            }
            case EUpdateOp::Remove:
            {
                auto it = m_entityMap.find(cmd.entityId);
                if (it != m_entityMap.end())
                {
                    m_entityMap.erase(it);
                    m_entityCount--;
                    m_batchRenderer.removeFromBatch(cmd.entityId);
                }
                break;
            }
        }
    }
}

void GL46Backend::defragment(std::span<const EntityId> keepIds)
{
    // 重建实体映射，重排顶点缓冲区
    std::vector<Vertex> compactedVertices;
    std::unordered_map<EntityId, EntityInfo> newMap;

    size_t currentOffset = 0;
    for (EntityId id : keepIds)
    {
        auto it = m_entityMap.find(id);
        if (it != m_entityMap.end())
        {
            // TODO: 需要从缓存获取顶点数据
            // 这里简化处理，实际需要存储完整顶点数据
            EntityInfo info = it->second;
            info.bufferOffset = currentOffset;
            newMap[id] = info;
            currentOffset += info.vertexCount;
        }
    }

    m_entityMap = std::move(newMap);
    m_dirtyEntities.clear();
}

void GL46Backend::setRenderState(const RenderState& state)
{
    m_state.renderState = state;
}

void GL46Backend::drawInstanced(size_t first, size_t count, EPrimitiveType primitiveType)
{
    glDrawArrays(toGLPrimitive(primitiveType), static_cast<GLint>(first),
                 static_cast<GLsizei>(count));
}

void GL46Backend::renderAll()
{
    glBindVertexArray(m_state.vao);
    glUseProgram(m_state.sceneProgram);

    // 设置Uniform
    glUniformMatrix3fv(m_state.locViewMatrix, 1, GL_FALSE,
                       &m_state.renderState.viewMatrix.data[0]);

    // 渲染批次
    m_batchRenderer.render(m_state.renderState);

    glUseProgram(0);
    glBindVertexArray(0);
}

size_t GL46Backend::getBufferMemoryUsage() const
{
    return m_state.vertexBufferMemory + m_state.indirectBufferMemory;
}

bool GL46Backend::compileShaders()
{
    // 创建顶点着色器
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VERTEX_SHADER_SOURCE, nullptr);
    glCompileShader(vs);

    GLint success;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(vs, 512, nullptr, infoLog);
        // log error
        glDeleteShader(vs);
        return false;
    }

    // 创建片段着色器
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FRAGMENT_SHADER_SOURCE, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(fs, 512, nullptr, infoLog);
        // log error
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    // 链接程序
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        // log error
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    // 清理中间对象
    glDeleteShader(vs);
    glDeleteShader(fs);

    m_state.sceneProgram = program;

    // 缓存Uniform位置
    m_state.locViewMatrix = glGetUniformLocation(program, "u_viewMatrix");
    m_state.locUniformColor = glGetUniformLocation(program, "u_uniformColor");
    m_state.locUseVertexColor = glGetUniformLocation(program, "u_useVertexColor");

    return true;
}

bool GL46Backend::createVertexArray()
{
    glGenVertexArrays(1, &m_state.vao);
    glBindVertexArray(m_state.vao);

    // 解耦顶点格式和缓冲区绑定
    // 使用ARB_direct_state_access可以直接创建VAO内容

    // 顶点格式说明：
    // location 0: position (vec3)
    // location 1: color (vec3)

    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, POSITION_OFFSET);
    glVertexAttribFormat(1, 3, GL_FLOAT, GL_FALSE, COLOR_OFFSET);

    // 绑定到顶点缓冲区
    glVertexAttribBinding(0, 0);
    glVertexAttribBinding(1, 0);

    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);

    glBindVertexBuffer(0, m_state.vertexBuffer.getHandle(), 0, VERTEX_STRIDE);

    glBindVertexArray(0);

    return true;
}

bool GL46Backend::createIndirectBuffer()
{
    m_state.indirectBufferMemory = INITIAL_INDIRECT_CAPACITY * sizeof(DrawArraysIndirectCommand);
    return m_state.indirectBuffer.create(EBufferType::Indirect, EBufferUsage::Dynamic,
                                         m_state.indirectBufferMemory, nullptr);
}

void GL46Backend::ensureVertexCapacity(size_t requiredVertices)
{
    size_t currentCapacity = m_state.vertexBuffer.getSize() / sizeof(Vertex);
    if (requiredVertices > currentCapacity)
    {
        size_t newCapacity = static_cast<size_t>(requiredVertices * GROWTH_FACTOR);
        m_state.vertexBufferMemory = newCapacity * sizeof(Vertex);
        m_state.vertexBuffer.orphan(m_state.vertexBufferMemory, nullptr);
    }
}

void GL46Backend::uploadEntityData(const UpdateCommand& cmd)
{
    if (cmd.vertices.empty())
        return;

    auto it = m_entityMap.find(cmd.entityId);
    if (it == m_entityMap.end())
        return;

    EntityInfo& info = it->second;

    // 计算缓冲区偏移
    size_t offset = info.bufferOffset * sizeof(Vertex);
    size_t size = cmd.vertices.size() * sizeof(Vertex);

    // 确保容量足够
    ensureVertexCapacity(info.bufferOffset + cmd.vertices.size());

    // 上传数据
    m_state.vertexBuffer.upload(offset, size, cmd.vertices.data());
}

} // namespace RenderCore
