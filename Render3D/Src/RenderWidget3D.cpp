#include "Render3D/RenderWidget3D.h"
#include "Render3D/GizmoRenderer.h"
#include "ShaderDef.h"
#include "ShaderManager.h"
#include "Engine3D/SceneManager3D.h"
#include "Engine3D/SyEntity/SyMeshEntity.h"
#include "Engine3D/Selection/SelectionManager3D.h"
#include "Log/SyLogger.h"

#include <QDebug>
#include <QOpenGLFunctions>
#include <QKeyEvent>

RenderWidget3D::RenderWidget3D(QWidget* parent)
    : QOpenGLWidget(parent)
{
    qDebug() << "[RenderWidget3D] Constructor called";
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);

    m_gizmoRenderer = std::make_unique<GizmoRenderer>();

    // 连接选择变化信号
    m_selectionManager.onSelectionChanged = [this](const auto& entities) {
        qDebug() << "[RenderWidget3D] Selection changed callback triggered, entities count:" << entities.size();
        emit sigSelectionChanged(entities);
        update();
    };

    // 连接变换变化信号（实时刷新）
    m_selectionManager.onTransformChanged = [this]() {
        update();
    };
}

RenderWidget3D::~RenderWidget3D()
{
    doneCurrent();
}

void RenderWidget3D::setSceneManager(Eg::SceneManager3D* sceneManager)
{
    m_sceneManager = sceneManager;
    m_selectionManager.setSceneManager(sceneManager);
    m_meshCacheDirty = true;  // 场景切换时清除 GPU 缓存
}

Eg::SelectionManager3D& RenderWidget3D::selectionManager()
{
    return m_selectionManager;
}

const Eg::SelectionManager3D& RenderWidget3D::selectionManager() const
{
    return m_selectionManager;
}

void RenderWidget3D::setTransformMode(Eg::TransformMode mode)
{
    m_selectionManager.setTransformMode(mode);
    update();
}

Eg::TransformMode RenderWidget3D::getTransformMode() const
{
    return m_selectionManager.getTransformMode();
}

Camera3D& RenderWidget3D::camera()
{
    return m_camera;
}
const Camera3D& RenderWidget3D::camera() const
{
    return m_camera;
}

void RenderWidget3D::resetView()
{
    m_camera.reset();
    update();
    emit sigCameraChanged();
}

void RenderWidget3D::fitAll()
{
    if (m_sceneManager && !m_sceneManager->isEmpty())
    {
        Ut::BBox3f bbox = m_sceneManager->getSceneBBox();
        if (bbox.isValid())
        {
            m_camera.focusOnBBox(bbox.minPt, bbox.maxPt);
            update();
            emit sigCameraChanged();
        }
    }
}

void RenderWidget3D::setViewPreset(Camera3D::ViewPreset preset)
{
    m_camera.setViewPreset(preset);
    update();
    emit sigCameraChanged();
}

void RenderWidget3D::setWireframeMode(bool enabled)
{
    m_wireframeMode = enabled;
    update();
}

bool RenderWidget3D::isWireframeMode() const
{
    return m_wireframeMode;
}

void RenderWidget3D::setShowGrid(bool visible)
{
    m_showGrid = visible;
    update();
}

void RenderWidget3D::setShowBBox(bool visible)
{
    m_showBBox = visible;
    update();
}

void RenderWidget3D::setShowFloor(bool visible)
{
    m_showFloor = visible;
    update();
}

bool RenderWidget3D::isShowFloor() const
{
    return m_showFloor;
}

bool RenderWidget3D::isEntitySelected(const Eg::SyMeshEntity* entity) const
{
    if (!entity) return false;
    const auto& selected = m_selectionManager.getSelectedEntities();
    return std::find(selected.begin(), selected.end(), entity) != selected.end();
}

// ==================== OpenGL 初始化 ====================

void RenderWidget3D::initializeGL()
{
    initializeOpenGLFunctions();

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_MULTISAMPLE);

    glClearColor(0.15f, 0.15f, 0.18f, 1.0f);

    initShaders();
    initGridGeometry();
    initFloorGeometry();
    initBBoxGeometry();

    m_gizmoRenderer->initialize();
}

void RenderWidget3D::resizeGL(int w, int h)
{
    m_aspectRatio = float(w) / float(std::max(h, 1));
    glViewport(0, 0, w, h);
}

void RenderWidget3D::initShaders()
{
    m_meshProgram = new QOpenGLShaderProgram(this);
    if (!m_meshProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::MESH3D_VS))
    {
        qWarning() << "Mesh VS compile error:" << m_meshProgram->log();
        SY_WARNF("[RenderWidget3D] Mesh VS compile error: %s", qPrintable(m_meshProgram->log()));
    }
    if (!m_meshProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::MESH3D_FS))
    {
        qWarning() << "Mesh FS compile error:" << m_meshProgram->log();
        SY_WARNF("[RenderWidget3D] Mesh FS compile error: %s", qPrintable(m_meshProgram->log()));
    }
    m_meshProgram->link();

    m_gridProgram = new QOpenGLShaderProgram(this);
    m_gridProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::GRID3D_VS);
    m_gridProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::GRID3D_FS);
    m_gridProgram->link();

    m_bboxProgram = new QOpenGLShaderProgram(this);
    m_bboxProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::BBOX3D_VS);
    m_bboxProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::BBOX3D_FS);
    m_bboxProgram->link();

    m_highlightProgram = new QOpenGLShaderProgram(this);
    m_highlightProgram->addShaderFromSourceCode(QOpenGLShader::Vertex, Shaders::HIGHLIGHT3D_VS);
    m_highlightProgram->addShaderFromSourceCode(QOpenGLShader::Fragment, Shaders::HIGHLIGHT3D_FS);
    m_highlightProgram->link();
}

void RenderWidget3D::initGridGeometry()
{
    const int gridSize = 20;
    const float spacing = 1.0f;
    const float extent = gridSize * spacing;

    std::vector<float> vertices;
    for (int i = -gridSize; i <= gridSize; ++i)
    {
        float pos = i * spacing;
        vertices.insert(vertices.end(), { pos, 0.0f, -extent, pos, 0.0f, extent });
        vertices.insert(vertices.end(), { -extent, 0.0f, pos, extent, 0.0f, pos });
    }
    m_gridVertexCount = static_cast<int>(vertices.size() / 3);

    glGenVertexArrays(1, &m_gridVao);
    glGenBuffers(1, &m_gridVbo);

    glBindVertexArray(m_gridVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_gridVbo);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void RenderWidget3D::initFloorGeometry()
{
    const float floorSize = 20.0f;
    const float yOffset = -0.001f;

    float floor[] = {
        -floorSize, yOffset, -floorSize,
        floorSize, yOffset, -floorSize,
        floorSize, yOffset, floorSize,
        -floorSize, yOffset, floorSize,
    };

    m_floorVertexCount = 4;

    glGenVertexArrays(1, &m_floorVao);
    glGenBuffers(1, &m_floorVbo);

    glBindVertexArray(m_floorVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_floorVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(floor), floor, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

void RenderWidget3D::initBBoxGeometry()
{
    float bbox[] = {
        0,0,0, 1,0,0,  1,0,0, 1,0,1,  1,0,1, 0,0,1,  0,0,1, 0,0,0,
        0,1,0, 1,1,0,  1,1,0, 1,1,1,  1,1,1, 0,1,1,  0,1,1, 0,1,0,
        0,0,0, 0,1,0,  1,0,0, 1,1,0,  1,0,1, 1,1,1,  0,0,1, 0,1,1,
    };

    glGenVertexArrays(1, &m_bboxVao);
    glGenBuffers(1, &m_bboxVbo);

    glBindVertexArray(m_bboxVao);
    glBindBuffer(GL_ARRAY_BUFFER, m_bboxVbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(bbox), bbox, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
}

// ==================== 渲染 ====================

void RenderWidget3D::paintGL()
{
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (m_wireframeMode)
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    else
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    if (m_showFloor)
        renderFloor();

    if (m_showGrid)
        renderGrid();

    renderMeshEntities();

    if (m_showBBox && m_sceneManager && !m_sceneManager->isEmpty())
    {
        Ut::BBox3f bbox = m_sceneManager->getSceneBBox();
        if (bbox.isValid())
            renderBBox(bbox.minPt, bbox.maxPt);
    }

    if (m_showSelectionHighlight && m_selectionManager.hasSelection())
    {
        renderSelectionHighlight();
    }

    // 渲染Gizmo（在变换模式下，且选中实体时）
    if (m_selectionManager.getTransformMode() != Eg::TransformMode::None
        && m_selectionManager.hasSelection())
    {
        renderGizmo();
    }
}

void RenderWidget3D::renderMeshEntities()
{
    if (!m_sceneManager || !m_meshProgram) return;

    // 场景变化时清除缓存
    if (m_meshCacheDirty)
    {
        for (auto& [_, cache] : m_meshCache)
        {
            if (cache.vao) glDeleteVertexArrays(1, &cache.vao);
            if (cache.vbo) glDeleteBuffers(1, &cache.vbo);
            if (cache.ebo) glDeleteBuffers(1, &cache.ebo);
        }
        m_meshCache.clear();
        m_meshCacheDirty = false;
    }

    auto viewMat = m_camera.getViewMatrix();
    auto projMat = m_camera.getProjectionMatrix(m_aspectRatio);
    auto cameraPos = m_camera.getPosition();

    m_meshProgram->bind();

    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    GLint loc = m_meshProgram->uniformLocation("uView");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, viewMat.data);

    loc = m_meshProgram->uniformLocation("uProjection");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, projMat.data);

    m_meshProgram->setUniformValue("uLightPos", cameraPos[0], cameraPos[1] + 5.0f, cameraPos[2]);
    m_meshProgram->setUniformValue("uViewPos", cameraPos[0], cameraPos[1], cameraPos[2]);
    m_meshProgram->setUniformValue("uLightColor", 1.0f, 1.0f, 1.0f);
    m_meshProgram->setUniformValue("uAmbientStrength", 0.3f);

    for (const auto& entity : m_sceneManager->getAllEntities())
    {
        if (!entity || !entity->isValid()) continue;

        const Eg::SyMeshEntity* rawPtr = entity.get();
        auto& cache = m_meshCache[rawPtr];

        // 脏标记检测：仅在数据变化时重新上传 GPU
        if (cache.entityGeneration != rawPtr->generation || cache.vao == 0)
        {
            updateMeshBuffersCache(rawPtr, cache);
            cache.entityGeneration = rawPtr->generation;
        }

        if (cache.vao == 0) continue;

        // 模型矩阵：如果实体被选中且正在变换，应用变换矩阵
        Ut::Mat4f model;
        if (isEntitySelected(rawPtr) && m_selectionManager.isTransforming())
        {
            model = m_selectionManager.getTransformMatrix();
        }

        loc = m_meshProgram->uniformLocation("uModel");
        f->glUniformMatrix4fv(loc, 1, GL_FALSE, model.data);

        float normalMat[9] = {
            model(0, 0), model(0, 1), model(0, 2),
            model(1, 0), model(1, 1), model(1, 2),
            model(2, 0), model(2, 1), model(2, 2),
        };
        loc = m_meshProgram->uniformLocation("uNormalMatrix");
        f->glUniformMatrix3fv(loc, 1, GL_FALSE, normalMat);

        m_meshProgram->setUniformValue("uAmbientColor",
            rawPtr->ambientColor[0], rawPtr->ambientColor[1], rawPtr->ambientColor[2]);
        m_meshProgram->setUniformValue("uObjectColor",
            rawPtr->diffuseColor[0], rawPtr->diffuseColor[1], rawPtr->diffuseColor[2]);
        m_meshProgram->setUniformValue("uSpecularColor",
            rawPtr->specularColor[0], rawPtr->specularColor[1], rawPtr->specularColor[2]);
        m_meshProgram->setUniformValue("uShininess", rawPtr->shininess);

        glBindVertexArray(cache.vao);

        if (cache.indexCount > 0)
        {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cache.indexCount), GL_UNSIGNED_INT, 0);
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(cache.vertexCount));
        }
    }

    glBindVertexArray(0);
    m_meshProgram->release();
}

void RenderWidget3D::renderGrid()
{
    if (!m_gridProgram) return;

    auto viewMat = m_camera.getViewMatrix();
    auto projMat = m_camera.getProjectionMatrix(m_aspectRatio);

    m_gridProgram->bind();
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    GLint loc = m_gridProgram->uniformLocation("uView");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, viewMat.data);
    loc = m_gridProgram->uniformLocation("uProjection");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, projMat.data);
    m_gridProgram->setUniformValue("uGridColor", 0.4f, 0.4f, 0.4f);
    m_gridProgram->setUniformValue("uAxisColor", 0.6f, 0.6f, 0.6f);
    m_gridProgram->setUniformValue("uGridSize", 1.0f);

    glBindVertexArray(m_gridVao);
    glLineWidth(1.0f);
    glDrawArrays(GL_LINES, 0, m_gridVertexCount);
    m_gridProgram->release();
}

void RenderWidget3D::renderFloor()
{
    if (!m_meshProgram) return;

    auto viewMat = m_camera.getViewMatrix();
    auto projMat = m_camera.getProjectionMatrix(m_aspectRatio);

    m_meshProgram->bind();
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    GLint loc = m_meshProgram->uniformLocation("uView");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, viewMat.data);
    loc = m_meshProgram->uniformLocation("uProjection");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, projMat.data);

    Ut::Mat4f model;
    loc = m_meshProgram->uniformLocation("uModel");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, model.data);

    float normalMat[9] = { 1,0,0, 0,1,0, 0,0,1 };
    loc = m_meshProgram->uniformLocation("uNormalMatrix");
    f->glUniformMatrix3fv(loc, 1, GL_FALSE, normalMat);

    m_meshProgram->setUniformValue("uAmbientColor", 0.2f, 0.25f, 0.3f);
    m_meshProgram->setUniformValue("uObjectColor", 0.15f, 0.2f, 0.25f);
    m_meshProgram->setUniformValue("uSpecularColor", 0.0f, 0.0f, 0.0f);
    m_meshProgram->setUniformValue("uShininess", 1.0f);
    m_meshProgram->setUniformValue("uAmbientStrength", 1.0f);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_floorVao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, m_floorVertexCount);
    glBindVertexArray(0);

    glDisable(GL_BLEND);
    m_meshProgram->release();
}

void RenderWidget3D::renderBBox(const Ut::Vec3f& bboxMin, const Ut::Vec3f& bboxMax)
{
    if (!m_bboxProgram) return;

    auto viewMat = m_camera.getViewMatrix();
    auto projMat = m_camera.getProjectionMatrix(m_aspectRatio);

    m_bboxProgram->bind();
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();
    GLint loc = m_bboxProgram->uniformLocation("uView");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, viewMat.data);
    loc = m_bboxProgram->uniformLocation("uProjection");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, projMat.data);
    m_bboxProgram->setUniformValue("uColor", 0.2f, 0.6f, 1.0f, 0.8f);

    glBindVertexArray(m_bboxVao);
    glLineWidth(1.5f);
    glDrawArrays(GL_LINES, 0, 24);
    m_bboxProgram->release();
}

void RenderWidget3D::renderSelectionHighlight()
{
    if (!m_sceneManager || !m_selectionManager.hasSelection()) return;

    // 优先使用专用高亮着色器（边缘发光效果），回退到 Phong 线框模式
    QOpenGLShaderProgram* highlightProg = m_highlightProgram;
    if (!highlightProg)
    {
        highlightProg = m_meshProgram;
    }
    if (!highlightProg) return;

    auto viewMat = m_camera.getViewMatrix();
    auto projMat = m_camera.getProjectionMatrix(m_aspectRatio);

    highlightProg->bind();
    QOpenGLFunctions* f = QOpenGLContext::currentContext()->functions();

    GLint loc = highlightProg->uniformLocation("uView");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, viewMat.data);

    loc = highlightProg->uniformLocation("uProjection");
    f->glUniformMatrix4fv(loc, 1, GL_FALSE, projMat.data);

    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    glLineWidth(2.5f);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (highlightProg == m_highlightProgram)
    {
        // 边缘发光高亮效果
        highlightProg->setUniformValue("uGlowColor", 0.0f, 1.0f, 0.5f);
        highlightProg->setUniformValue("uGlowIntensity", 1.5f);
        highlightProg->setUniformValue("uGlowWidth", 0.02f);
    }
    else
    {
        // 回退：Phong 线框模式
        highlightProg->setUniformValue("uAmbientColor", 0.0f, 1.0f, 0.5f);
        highlightProg->setUniformValue("uObjectColor", 0.0f, 1.0f, 0.5f);
        highlightProg->setUniformValue("uSpecularColor", 0.0f, 0.0f, 0.0f);
        highlightProg->setUniformValue("uShininess", 1.0f);
        highlightProg->setUniformValue("uAmbientStrength", 1.0f);
    }

    const auto& selectedEntities = m_selectionManager.getSelectedEntities();
    for (Eg::SyMeshEntity* entity : selectedEntities)
    {
        if (!entity || !entity->isValid()) continue;

        auto& cache = m_meshCache[entity];
        if (cache.entityGeneration != entity->generation || cache.vao == 0)
        {
            updateMeshBuffersCache(entity, cache);
            cache.entityGeneration = entity->generation;
        }

        if (cache.vao == 0) continue;

        Ut::Mat4f model;
        if (m_selectionManager.isTransforming())
        {
            model = m_selectionManager.getTransformMatrix();
        }

        loc = highlightProg->uniformLocation("uModel");
        f->glUniformMatrix4fv(loc, 1, GL_FALSE, model.data);

        float normalMat[9] = { 1,0,0, 0,1,0, 0,0,1 };
        loc = highlightProg->uniformLocation("uNormalMatrix");
        f->glUniformMatrix3fv(loc, 1, GL_FALSE, normalMat);

        glBindVertexArray(cache.vao);
        if (cache.indexCount > 0)
        {
            glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(cache.indexCount), GL_UNSIGNED_INT, 0);
        }
        else
        {
            glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(cache.vertexCount));
        }
    }

    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    highlightProg->release();
}

void RenderWidget3D::renderGizmo()
{
    if (!m_gizmoRenderer || !m_selectionManager.hasSelection())
        return;

    Ut::Vec3f pivot = m_selectionManager.getSelectionCenter();
    float gizmoSize = 1.0f;

    // 根据包围盒大小调整Gizmo大小
    Ut::BBox3f bbox = m_selectionManager.getSelectionBBox();
    if (bbox.isValid())
    {
        float bboxSize = (bbox.maxPt - bbox.minPt).length();
        gizmoSize = bboxSize * 0.5f;
        if (gizmoSize < 0.5f) gizmoSize = 0.5f;
        if (gizmoSize > 5.0f) gizmoSize = 5.0f;
    }

    m_gizmoRenderer->render(
        m_selectionManager.getTransformMode(),
        pivot,
        m_camera,
        m_aspectRatio,
        gizmoSize);
}

void RenderWidget3D::updateMeshBuffersCache(const Eg::SyMeshEntity* entity,
    MeshGPUCache& cache)
{
    if (!entity || entity->vertices.empty()) return;

    std::vector<float> buffer;
    buffer.reserve(entity->vertices.size() * 6);
    for (size_t i = 0; i < entity->vertices.size(); ++i)
    {
        buffer.push_back(entity->vertices[i][0]);
        buffer.push_back(entity->vertices[i][1]);
        buffer.push_back(entity->vertices[i][2]);
        if (i < entity->normals.size())
        {
            buffer.push_back(entity->normals[i][0]);
            buffer.push_back(entity->normals[i][1]);
            buffer.push_back(entity->normals[i][2]);
        }
        else
        {
            buffer.push_back(0.0f);
            buffer.push_back(1.0f);
            buffer.push_back(0.0f);
        }
    }

    size_t requiredBytes = buffer.size() * sizeof(float);

    // 首次创建或扩容
    if (cache.vao == 0)
    {
        glGenVertexArrays(1, &cache.vao);
        glGenBuffers(1, &cache.vbo);
    }

    glBindVertexArray(cache.vao);
    glBindBuffer(GL_ARRAY_BUFFER, cache.vbo);

    if (requiredBytes > cache.vboCapacity)
    {
        // Orphan + reallocate
        glBufferData(GL_ARRAY_BUFFER, requiredBytes, buffer.data(), GL_DYNAMIC_DRAW);
        cache.vboCapacity = requiredBytes;
    }
    else
    {
        // In-place update (no reallocation)
        glBufferSubData(GL_ARRAY_BUFFER, 0, requiredBytes, buffer.data());
    }

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindVertexArray(0);

    cache.vertexCount = entity->vertices.size();
    cache.indexCount = 0;
}

void RenderWidget3D::updateMeshBuffers(const Eg::SyMeshEntity* entity,
    GLuint& vao, GLuint& vbo, GLuint& ebo,
    size_t& vertexCount, size_t& indexCount)
{
    // 重构：不再使用静态局部队列，改为使用 per-entity 缓存
    // 此函数保留以兼容旧接口，内部委托给缓存版本
    if (!entity) return;
    auto& cache = m_meshCache[entity];
    if (cache.entityGeneration != entity->generation || cache.vao == 0)
    {
        updateMeshBuffersCache(entity, cache);
        cache.entityGeneration = entity->generation;
    }
    vao = cache.vao;
    vbo = cache.vbo;
    ebo = cache.ebo;
    vertexCount = cache.vertexCount;
    indexCount = cache.indexCount;
}

// ==================== 鼠标交互 ====================

void RenderWidget3D::mousePressEvent(QMouseEvent* event)
{
    m_lastMousePos = event->pos();
    m_pressedButtons = event->buttons();

    if (event->button() == Qt::LeftButton)
    {
        // 如果有选中的实体且处于变换模式，开始变换
        if (m_selectionManager.hasSelection() &&
            m_selectionManager.getTransformMode() != Eg::TransformMode::None)
        {
            m_interactionMode = InteractionMode::Transform;
            Eg::Ray3f ray = screenToWorldRay(event->x(), event->y());
            m_selectionManager.beginTransform(ray);
        }
        else
        {
            // 否则进行选择
            m_interactionMode = InteractionMode::Select;
            Eg::Ray3f ray = screenToWorldRay(event->x(), event->y());
            m_selectionManager.selectByRay(ray, m_ctrlPressed);
        }
    }
    else if (event->button() == Qt::MiddleButton)
    {
        m_interactionMode = InteractionMode::Pan;
    }
    else if (event->button() == Qt::RightButton)
    {
        m_interactionMode = InteractionMode::Rotate;
    }

    event->accept();
}

void RenderWidget3D::mouseMoveEvent(QMouseEvent* event)
{
    QPoint delta = event->pos() - m_lastMousePos;
    m_lastMousePos = event->pos();

    if (m_interactionMode == InteractionMode::Rotate)
    {
        m_camera.rotate(static_cast<float>(delta.x()), static_cast<float>(-delta.y()));
        update();
        emit sigCameraChanged();
    }
    else if (m_interactionMode == InteractionMode::Pan)
    {
        m_camera.pan(static_cast<float>(delta.x()), static_cast<float>(delta.y()));
        update();
        emit sigCameraChanged();
    }
    else if (m_interactionMode == InteractionMode::Transform)
    {
        Eg::Ray3f ray = screenToWorldRay(event->x(), event->y());
        Ut::Vec3f cameraForward = m_camera.getForward();
        m_selectionManager.updateTransform(ray, cameraForward);
        // update() 由 onTransformChanged 回调触发
    }

    event->accept();
}

void RenderWidget3D::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_interactionMode == InteractionMode::Transform)
    {
        m_selectionManager.endTransform();
        m_selectionManager.applyTransform();
        update();
    }

    m_interactionMode = InteractionMode::None;
    m_pressedButtons = event->buttons();
    event->accept();
}

void RenderWidget3D::wheelEvent(QWheelEvent* event)
{
    float delta = static_cast<float>(event->angleDelta().y()) / 120.0f;
    m_camera.zoom(delta);
    update();
    emit sigCameraChanged();
    event->accept();
}

void RenderWidget3D::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Control)
    {
        m_ctrlPressed = true;
    }
    else if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
    {
        if (m_selectionManager.hasSelection() && m_sceneManager)
        {
            const auto& selected = m_selectionManager.getSelectedEntities();
            for (Eg::SyMeshEntity* entity : selected)
            {
                m_sceneManager->removeEntity(entity);
            }
            m_selectionManager.clearSelection();
            update();
        }
    }
    else if (event->key() == Qt::Key_T)
    {
        m_selectionManager.setTransformMode(Eg::TransformMode::Translate);
        update();
    }
    else if (event->key() == Qt::Key_R)
    {
        m_selectionManager.setTransformMode(Eg::TransformMode::Rotate);
        update();
    }
    else if (event->key() == Qt::Key_S)
    {
        m_selectionManager.setTransformMode(Eg::TransformMode::Scale);
        update();
    }
    else if (event->key() == Qt::Key_Escape)
    {
        m_selectionManager.clearSelection();
        m_selectionManager.setTransformMode(Eg::TransformMode::None);
        update();
    }

    QOpenGLWidget::keyPressEvent(event);
}

void RenderWidget3D::keyReleaseEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Control)
    {
        m_ctrlPressed = false;
    }

    QOpenGLWidget::keyReleaseEvent(event);
}

// ==================== 辅助方法 ====================

Eg::Ray3f RenderWidget3D::screenToWorldRay(int x, int y) const
{
    float ndcX = (2.0f * x) / width() - 1.0f;
    float ndcY = 1.0f - (2.0f * y) / height();

    Ut::Vec3f cameraPos = m_camera.getPosition();
    Ut::Vec3f forward = m_camera.getForward();
    Ut::Vec3f right = m_camera.getRight();
    Ut::Vec3f up = m_camera.getUp();

    float fovRad = m_camera.getFov() * 3.1415926f / 180.0f;
    float halfHeight = tan(fovRad / 2.0f) * 0.1f;
    float halfWidth = halfHeight * m_aspectRatio;

    Ut::Vec3f viewCenter = cameraPos + forward * 0.1f;
    Ut::Vec3f viewPoint = viewCenter + right * ndcX * halfWidth + up * ndcY * halfHeight;

    Ut::Vec3f direction = (viewPoint - cameraPos).normalized();
    return Eg::Ray3f(cameraPos, direction);
}