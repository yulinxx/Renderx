#include "RenderCore/RenderWidgetEx.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>

#include <iostream>

namespace RenderCore
{

// ==================== 着色器源码 ========== =========

static const char* VERTEX_SHADER_2D = R"(
#version 450 core

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_color;

layout(binding = 0) uniform ViewMatrix
{
    mat3 u_viewMatrix;
};

out vec3 v_color;

void main()
{
    vec2 pos = u_viewMatrix * vec3(a_position.xy, 1.0);
    gl_Position = vec4(pos, 0.0, 1.0);
    v_color = a_color;
}
)";

static const char* FRAGMENT_SHADER_2D = R"(
#version 450 core

in vec3 v_color;
out vec4 fragColor;

void main()
{
    fragColor = vec4(v_color, 1.0);
}
)";

// ==================== RenderWidgetEx 实现 ========== =========

RenderWidgetEx::RenderWidgetEx(QWidget* parent,
                                ViewManager* viewManager,
                                EViewType viewType)
    : QOpenGLWidget(parent)
    , m_viewManager(viewManager)
    , m_viewType(viewType)
{
    // 初始化视图矩阵为单位矩阵
    m_viewMatrix = Ut::Mat3f::identity();

    // 设置背景色
    setBackgroundRole(QPalette::NoRole);
    setAutoFillBackground(false);

    // 启用鼠标追踪
    setMouseTracking(true);

    // 设置更新定时器
    m_updateTimer.setInterval(1000 / m_targetFPS);
    connect(&m_updateTimer, &QTimer::timeout, this, &RenderWidgetEx::requestUpdate);
}

RenderWidgetEx::~RenderWidgetEx()
{
    m_updateTimer.stop();

    makeCurrent();

    if (m_sceneProgram)
    {
        glDeleteProgram(m_sceneProgram);
    }

    if (m_vao)
    {
        glDeleteVertexArrays(1, &m_vao);
    }

    doneCurrent();
}

void RenderWidgetEx::initializeGL()
{
    // 初始化OpenGL函数
    if (!initializeOpenGLFunctions())
    {
        std::cerr << "Failed to initialize OpenGL functions" << std::endl;
        return;
    }

    // 打印OpenGL版本
    const GLubyte* version = glGetString(GL_VERSION);
    const GLubyte* renderer = glGetString(GL_RENDERER);
    std::cout << "OpenGL Version: " << version << std::endl;
    std::cout << "Renderer: " << renderer << std::endl;

    // 编译着色器
    initShaders();

    // 创建VAO
    glGenVertexArrays(1, &m_vao);

    // 注册到视图管理器
    if (m_viewManager)
    {
        m_viewManager->registerView(this);
    }

    // 启动更新定时器
    m_updateTimer.start();
}

void RenderWidgetEx::resizeGL(int w, int h)
{
    m_viewportWidth = w;
    m_viewportHeight = h;
    onResize(w, h);
}

void RenderWidgetEx::paintGL()
{
    if (!m_viewManager)
        return;

    RenderWorld* world = m_viewManager->getWorld();
    if (!world)
        return;

    // 清除
    glClearColor(1.0f, 1.0f, 1.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // 设置视口
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);

    // 渲染世界
    RenderState state = getRenderState();
    world->render(state);
}

void RenderWidgetEx::initShaders()
{
    // 创建顶点着色器
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VERTEX_SHADER_2D, nullptr);
    glCompileShader(vs);

    GLint success;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(vs, 512, nullptr, infoLog);
        std::cerr << "Vertex shader compile error: " << infoLog << std::endl;
        glDeleteShader(vs);
        return;
    }

    // 创建片段着色器
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &FRAGMENT_SHADER_2D, nullptr);
    glCompileShader(fs);

    glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(fs, 512, nullptr, infoLog);
        std::cerr << "Fragment shader compile error: " << infoLog << std::endl;
        glDeleteShader(vs);
        glDeleteShader(fs);
        return;
    }

    // 链接程序
    m_sceneProgram = glCreateProgram();
    glAttachShader(m_sceneProgram, vs);
    glAttachShader(m_sceneProgram, fs);
    glLinkProgram(m_sceneProgram);

    glGetProgramiv(m_sceneProgram, GL_LINK_STATUS, &success);
    if (!success)
    {
        char infoLog[512];
        glGetShaderInfoLog(m_sceneProgram, 512, nullptr, infoLog);
        std::cerr << "Program link error: " << infoLog << std::endl;
    }

    // 缓存Uniform位置
    m_locViewMatrix = glGetUniformLocation(m_sceneProgram, "u_viewMatrix");

    // 清理中间对象
    glDeleteShader(vs);
    glDeleteShader(fs);
}

void RenderWidgetEx::initGeometryBuffers()
{
    // VAO已在initializeGL中创建
}

void RenderWidgetEx::resetView()
{
    m_viewMatrix = Ut::Mat3f::identity();
    m_scale = 1.0f;
    m_translation = QPointF(0, 0);
    update();
}

void RenderWidgetEx::zoom(float factor)
{
    m_scale *= factor;
    updateViewMatrix();
}

void RenderWidgetEx::pan(float dx, float dy)
{
    m_translation += QPointF(dx, dy);
    updateViewMatrix();
}

RenderState RenderWidgetEx::getRenderState() const
{
    RenderState state;
    state.viewMatrix = m_viewMatrix;
    state.lineWidth = 1.0f;
    state.pointSize = 1.0f;
    state.depthTest = false;
    state.blending = false;
    return state;
}

void RenderWidgetEx::requestUpdate()
{
    if (!m_updateTimer.isActive())
        m_updateTimer.start();

    update();
}

void RenderWidgetEx::onResize(int width, int height)
{
    m_viewportWidth = width;
    m_viewportHeight = height;

    // 更新投影矩阵
    // 对于2D视图，这里设置正交投影
    // 对于3D视图，需要更新透视投影
}

void RenderWidgetEx::setEntity(EntityId id,
                                std::vector<Vertex> vertices,
                                EPrimitiveType primitiveType,
                                float lineWidth)
{
    if (m_viewManager)
    {
        RenderWorld* world = m_viewManager->getWorld();
        if (world)
        {
            world->setEntity(id, std::move(vertices), primitiveType, lineWidth);
            emit entityCountChanged(world->getEntityCount());
        }
    }
}

void RenderWidgetEx::setEntities(std::span<const EntityId> ids,
                                 std::span<const std::vector<Vertex>> vertices,
                                 std::span<const EPrimitiveType> primitiveTypes,
                                 std::span<const float> lineWidths)
{
    if (m_viewManager)
    {
        RenderWorld* world = m_viewManager->getWorld();
        if (world)
        {
            world->setEntities(ids, vertices, primitiveTypes, lineWidths);
            emit entityCountChanged(world->getEntityCount());
        }
    }
}

void RenderWidgetEx::removeEntity(EntityId id)
{
    if (m_viewManager)
    {
        RenderWorld* world = m_viewManager->getWorld();
        if (world)
        {
            world->removeEntity(id);
            emit entityCountChanged(world->getEntityCount());
        }
    }
}

void RenderWidgetEx::removeEntities(std::span<const EntityId> ids)
{
    if (m_viewManager)
    {
        RenderWorld* world = m_viewManager->getWorld();
        if (world)
        {
            world->removeEntities(ids);
            emit entityCountChanged(world->getEntityCount());
        }
    }
}

void RenderWidgetEx::clearEntities()
{
    if (m_viewManager)
    {
        RenderWorld* world = m_viewManager->getWorld();
        if (world)
        {
            world->clear();
            emit entityCountChanged(0);
        }
    }
}

} // namespace RenderCore
