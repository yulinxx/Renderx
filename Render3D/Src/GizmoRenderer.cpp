/**
 * @file GizmoRenderer.cpp
 * @brief 3D 变换手柄渲染器实现
 */

#include "Render3D/GizmoRenderer.h"
#include "ShaderDef.h"

#include <QDebug>
#include <QOpenGLContext>
#include <cmath>
#include <vector>

// 简单的手柄着色器
static const char* GIZMO_VS = R"(
#version 330 core
layout(location = 0) in vec3 aPos;

uniform mat4 uModel;
uniform mat4 uViewProj;

void main()
{
    gl_Position = uViewProj * uModel * vec4(aPos, 1.0);
}
)";

static const char* GIZMO_FS = R"(
#version 330 core
out vec4 FragColor;

uniform vec3 uColor;
uniform float uAlpha;

void main()
{
    FragColor = vec4(uColor, uAlpha);
}
)";

GizmoRenderer::GizmoRenderer()
{
}

GizmoRenderer::~GizmoRenderer()
{
    cleanup();
}

void GizmoRenderer::initialize()
{
    initializeOpenGLFunctions();
    initShader();
    createArrowGeometry();
    createRingGeometry();
    createCubeGeometry();
}

void GizmoRenderer::initShader()
{
    m_shader = new QOpenGLShaderProgram();
    if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Vertex, GIZMO_VS))
    {
        qWarning() << "[GizmoRenderer] VS compile error:" << m_shader->log();
    }
    if (!m_shader->addShaderFromSourceCode(QOpenGLShader::Fragment, GIZMO_FS))
    {
        qWarning() << "[GizmoRenderer] FS compile error:" << m_shader->log();
    }
    m_shader->link();
}

void GizmoRenderer::createArrowGeometry()
{
    // 箭头 = 圆柱体(杆) + 圆锥体(箭头)
    std::vector<float> vertices;

    const int segments = 8;
    const float shaftLength = 0.8f;
    const float shaftRadius = 0.03f;
    const float headLength = 0.2f;
    const float headRadius = 0.08f;

    // 箭杆
    for (int i = 0; i < segments; ++i)
    {
        float angle = static_cast<float>(i) * 2.0f * 3.1415926f / static_cast<float>(segments);
        float x = std::cos(angle) * shaftRadius;
        float y = std::sin(angle) * shaftRadius;

        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(0.0f);

        vertices.push_back(x);
        vertices.push_back(y);
        vertices.push_back(shaftLength);
    }

    // 箭头三角形
    for (int i = 0; i < segments; ++i)
    {
        float angle1 = static_cast<float>(i) * 2.0f * 3.1415926f / static_cast<float>(segments);
        float angle2 = static_cast<float>(i + 1) * 2.0f * 3.1415926f / static_cast<float>(segments);

        float x1 = std::cos(angle1) * headRadius;
        float y1 = std::sin(angle1) * headRadius;
        float x2 = std::cos(angle2) * headRadius;
        float y2 = std::sin(angle2) * headRadius;

        vertices.insert(vertices.end(), { x1, y1, shaftLength });
        vertices.insert(vertices.end(), { x2, y2, shaftLength });
        vertices.insert(vertices.end(), { 0.0f, 0.0f, shaftLength + headLength });
    }

    m_arrowVertexCount = static_cast<int>(vertices.size() / 3);

    glGenVertexArrays(1, &m_arrowVao);
    glGenBuffers(1, &m_arrowVbo);

    glBindVertexArray(m_arrowVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_arrowVbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void GizmoRenderer::createRingGeometry()
{
    std::vector<float> vertices;

    const int segments = 64;
    const float radius = 0.6f;

    for (int i = 0; i < segments; ++i)
    {
        float angle1 = static_cast<float>(i) * 2.0f * 3.1415926f / static_cast<float>(segments);
        float angle2 = static_cast<float>(i + 1) * 2.0f * 3.1415926f / static_cast<float>(segments);

        float x1 = std::cos(angle1) * radius;
        float y1 = std::sin(angle1) * radius;
        float x2 = std::cos(angle2) * radius;
        float y2 = std::sin(angle2) * radius;

        vertices.insert(vertices.end(), { x1, y1, 0.0f });
        vertices.insert(vertices.end(), { x2, y2, 0.0f });
    }

    m_ringVertexCount = static_cast<int>(vertices.size() / 3);

    glGenVertexArrays(1, &m_ringVao);
    glGenBuffers(1, &m_ringVbo);

    glBindVertexArray(m_ringVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_ringVbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void GizmoRenderer::createCubeGeometry()
{
    // 单位立方体的12条边
    float half = 0.06f;
    float vertices[] = {
        // 底面
        -half, -half, -half,  half, -half, -half,
         half, -half, -half,  half, -half,  half,
         half, -half,  half, -half, -half,  half,
        -half, -half,  half, -half, -half, -half,
        // 顶面
        -half,  half, -half,  half,  half, -half,
         half,  half, -half,  half,  half,  half,
         half,  half,  half, -half,  half,  half,
        -half,  half,  half, -half,  half, -half,
        // 竖边
        -half, -half, -half, -half,  half, -half,
         half, -half, -half,  half,  half, -half,
         half, -half,  half,  half,  half,  half,
        -half, -half,  half, -half,  half,  half,
    };

    m_cubeVertexCount = sizeof(vertices) / (3 * sizeof(float));

    glGenVertexArrays(1, &m_cubeVao);
    glGenBuffers(1, &m_cubeVbo);

    glBindVertexArray(m_cubeVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void GizmoRenderer::render(Eg::TransformMode mode,
    const Ut::Vec3f& position,
    const Camera3D& camera,
    float aspectRatio,
    float gizmoSize)
{
    if (!m_shader || !m_shader->isLinked())
        return;

    auto viewMat = camera.getViewMatrix();
    auto projMat = camera.getProjectionMatrix(aspectRatio);
    Ut::Mat4f viewProj = projMat * viewMat;

    m_shader->bind();
    GLint loc = m_shader->uniformLocation("uViewProj");
    glUniformMatrix4fv(loc, 1, GL_FALSE, viewProj.ptr());

    // 深度测试关闭，Gizmo始终可见
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    switch (mode)
    {
    case Eg::TransformMode::Translate:
    {
        renderArrow(position, Ut::Vec3f(1, 0, 0), Ut::Vec3f(1, 0, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::X);
        renderArrow(position, Ut::Vec3f(0, 1, 0), Ut::Vec3f(0, 1, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Y);
        renderArrow(position, Ut::Vec3f(0, 0, 1), Ut::Vec3f(0, 0, 1), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Z);
        break;
    }
    case Eg::TransformMode::Rotate:
    {
        renderRing(position, Ut::Vec3f(1, 0, 0), Ut::Vec3f(1, 0, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::X);
        renderRing(position, Ut::Vec3f(0, 1, 0), Ut::Vec3f(0, 1, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Y);
        renderRing(position, Ut::Vec3f(0, 0, 1), Ut::Vec3f(0, 0, 1), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Z);
        break;
    }
    case Eg::TransformMode::Scale:
    {
        renderCube(position + Ut::Vec3f(gizmoSize * 0.8f, 0, 0), Ut::Vec3f(1, 0, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::X);
        renderCube(position + Ut::Vec3f(0, gizmoSize * 0.8f, 0), Ut::Vec3f(0, 1, 0), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Y);
        renderCube(position + Ut::Vec3f(0, 0, gizmoSize * 0.8f), Ut::Vec3f(0, 0, 1), viewProj, gizmoSize,
            m_highlightAxis == Eg::TransformAxis::Z);
        break;
    }
    default:
        break;
    }

    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);

    m_shader->release();
}

void GizmoRenderer::renderArrow(const Ut::Vec3f& position,
    const Ut::Vec3f& direction,
    const Ut::Vec3f& color,
    const Ut::Mat4f& viewProj,
    float size,
    bool highlighted)
{
    Ut::Mat4f rotation = Ut::Mat4f::identity();
    Ut::Vec3f dirNorm = direction;
    if (dirNorm.length() < 1e-6f)
    {
        dirNorm = Ut::Vec3f(1, 0, 0);
    }
    else
    {
        dirNorm = dirNorm.normalized();
    }

    Ut::Vec3f right(1, 0, 0);
    float dotX = std::abs(dirNorm.x());
    float dotY = std::abs(dirNorm.y());
    float dotZ = std::abs(dirNorm.z());

    Ut::Vec3f crossResult;
    if (dotY >= dotX && dotY >= dotZ)
    {
        crossResult = Ut::Vec3f(1, 0, 0).cross(dirNorm);
    }
    else if (dotX >= dotZ)
    {
        crossResult = Ut::Vec3f(0, 1, 0).cross(dirNorm);
    }
    else
    {
        crossResult = Ut::Vec3f(0, 1, 0).cross(dirNorm);
    }

    if (crossResult.length() < 1e-6f)
    {
        right = Ut::Vec3f(1, 0, 0);
        if (std::abs(dirNorm.x()) > 0.9f)
            right = Ut::Vec3f(0, 0, 1);
    }
    else
    {
        right = crossResult.normalized();
    }

    Ut::Vec3f newUp = dirNorm.cross(right);
    if (newUp.length() < 1e-6f)
    {
        newUp = Ut::Vec3f(0, 1, 0);
        if (std::abs(dirNorm.y()) > 0.9f)
            newUp = Ut::Vec3f(0, 0, 1);
    }
    else
    {
        newUp = newUp.normalized();
    }

    rotation = Ut::Mat4f(
        right.x(), right.y(), right.z(), 0,
        newUp.x(), newUp.y(), newUp.z(), 0,
        dirNorm.x(), dirNorm.y(), dirNorm.z(), 0,
        0, 0, 0, 1
    );

    Ut::Mat4f model = Ut::Mat4f::translate(position) * rotation * Ut::Mat4f::scale(size);

    GLint loc = m_shader->uniformLocation("uModel");
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.ptr());

    float alpha = highlighted ? 1.0f : 0.7f;
    m_shader->setUniformValue("uColor", color[0], color[1], color[2]);
    m_shader->setUniformValue("uAlpha", alpha);

    glBindVertexArray(m_arrowVao);
    glLineWidth(highlighted ? 3.0f : 2.0f);
    glDrawArrays(GL_LINES, 0, m_arrowVertexCount);
    glBindVertexArray(0);
}

void GizmoRenderer::renderRing(const Ut::Vec3f& position,
    const Ut::Vec3f& normal,
    const Ut::Vec3f& color,
    const Ut::Mat4f& viewProj,
    float size,
    bool highlighted)
{
    Ut::Mat4f rotation = Ut::Mat4f::identity();
    Ut::Vec3f normNorm = normal;
    if (normNorm.length() < 1e-6f)
    {
        normNorm = Ut::Vec3f(0, 0, 1);
    }
    else
    {
        normNorm = normNorm.normalized();
    }

    Ut::Vec3f right(1, 0, 0);
    float dotX = std::abs(normNorm.x());
    float dotY = std::abs(normNorm.y());
    float dotZ = std::abs(normNorm.z());

    Ut::Vec3f crossResult;
    if (dotY >= dotX && dotY >= dotZ)
    {
        crossResult = Ut::Vec3f(1, 0, 0).cross(normNorm);
    }
    else if (dotX >= dotZ)
    {
        crossResult = Ut::Vec3f(0, 1, 0).cross(normNorm);
    }
    else
    {
        crossResult = Ut::Vec3f(0, 1, 0).cross(normNorm);
    }

    if (crossResult.length() < 1e-6f)
    {
        right = Ut::Vec3f(1, 0, 0);
        if (std::abs(normNorm.x()) > 0.9f)
            right = Ut::Vec3f(0, 0, 1);
    }
    else
    {
        right = crossResult.normalized();
    }

    Ut::Vec3f newUp = normNorm.cross(right);
    if (newUp.length() < 1e-6f)
    {
        newUp = Ut::Vec3f(0, 1, 0);
        if (std::abs(normNorm.y()) > 0.9f)
            newUp = Ut::Vec3f(0, 0, 1);
    }
    else
    {
        newUp = newUp.normalized();
    }

    rotation = Ut::Mat4f(
        right.x(), right.y(), right.z(), 0,
        newUp.x(), newUp.y(), newUp.z(), 0,
        normNorm.x(), normNorm.y(), normNorm.z(), 0,
        0, 0, 0, 1
    );

    Ut::Mat4f model = Ut::Mat4f::translate(position) * rotation * Ut::Mat4f::scale(size);

    GLint loc = m_shader->uniformLocation("uModel");
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.ptr());

    float alpha = highlighted ? 1.0f : 0.6f;
    m_shader->setUniformValue("uColor", color[0], color[1], color[2]);
    m_shader->setUniformValue("uAlpha", alpha);

    glBindVertexArray(m_ringVao);
    glLineWidth(highlighted ? 4.0f : 2.5f);
    glDrawArrays(GL_LINES, 0, m_ringVertexCount);
    glBindVertexArray(0);
}

void GizmoRenderer::renderCube(const Ut::Vec3f& position,
    const Ut::Vec3f& color,
    const Ut::Mat4f& viewProj,
    float size,
    bool highlighted)
{
    Ut::Mat4f model = Ut::Mat4f::translate(position) * Ut::Mat4f::scale(size);

    GLint loc = m_shader->uniformLocation("uModel");
    glUniformMatrix4fv(loc, 1, GL_FALSE, model.ptr());

    float alpha = highlighted ? 1.0f : 0.7f;
    m_shader->setUniformValue("uColor", color[0], color[1], color[2]);
    m_shader->setUniformValue("uAlpha", alpha);

    glBindVertexArray(m_cubeVao);
    glLineWidth(highlighted ? 3.0f : 2.0f);
    glDrawArrays(GL_LINES, 0, m_cubeVertexCount);
    glBindVertexArray(0);
}

void GizmoRenderer::setHighlightAxis(Eg::TransformAxis axis)
{
    m_highlightAxis = axis;
}

void GizmoRenderer::cleanup()
{
    if (QOpenGLContext::currentContext())
    {
        if (m_arrowVao) { glDeleteVertexArrays(1, &m_arrowVao); m_arrowVao = 0; }
        if (m_arrowVbo) { glDeleteBuffers(1, &m_arrowVbo); m_arrowVbo = 0; }
        if (m_ringVao) { glDeleteVertexArrays(1, &m_ringVao); m_ringVao = 0; }
        if (m_ringVbo) { glDeleteBuffers(1, &m_ringVbo); m_ringVbo = 0; }
        if (m_cubeVao) { glDeleteVertexArrays(1, &m_cubeVao); m_cubeVao = 0; }
        if (m_cubeVbo) { glDeleteBuffers(1, &m_cubeVbo); m_cubeVbo = 0; }
    }
    delete m_shader;
    m_shader = nullptr;
}