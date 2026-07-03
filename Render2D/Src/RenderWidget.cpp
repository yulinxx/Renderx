#include "Render2D/RenderWidget.h"
#include "ShaderDef.h"
#include "ShaderManager.h"
#include "Log/SyLogger.h"

#include <QMouseEvent>
#include <QPainter>
#include <QFontMetrics>
#include <QShowEvent>
#include <QOpenGLContext>
#include <algorithm>

RenderWidget::RenderWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    qDebug() << "[RenderWidget] Constructor called, parent =" << parent;
    init();
}

void RenderWidget::init()
{
    // 使用全局默认格式 (在 main.cpp 中通过 QSurfaceFormat::setDefaultFormat 设置)，避免重复设置导致冲突
    qDebug() << "[RenderWidget::init] Using global default QSurfaceFormat (skip local setFormat)";
    // QSurfaceFormat format;
    // format.setVersion(TARGET_GL_VERSION_MAJOR, TARGET_GL_VERSION_MINOR);
    // format.setProfile(QSurfaceFormat::CoreProfile);
    // format.setDepthBufferSize(24);
    // setFormat(format);

    m_crossPoints = {
        Render::Vec2f(-0.9f, 0.0f), Render::Vec2f(0.9f, 0.0f),
        Render::Vec2f(0.0f, -0.9f), Render::Vec2f(0.0f, 0.9f)
    };

    m_viewMatrix = Ut::Mat3f::identity();
    m_drawingColor = QColor(0, 0, 255);
}

RenderWidget::~RenderWidget()
{
    // 关键：使用独立的 XGLFunctions 指针对象而非继承，
    // 避免在 context 已失效时析构触发 ASSERT。
    //
    // QOpenGLShaderProgram 的 unique_ptr 自动析构是安全的：
    // - 如果 context 有效，shader program 对象会调用 glDeleteProgram
    // - 如果 context 已失效，Qt 内部会跳过 GL 调用
    // - m_glFuncs 由我们自己管理，在 releaseGLResources() 中 delete
}

void RenderWidget::releaseGLResources()
{
    qDebug() << "[RenderWidget] releaseGLResources() ENTER, m_glResourcesReleased =" << m_glResourcesReleased
        << "m_glInitialized =" << m_glInitialized;

    if (m_glResourcesReleased)
    {
        qDebug() << "[RenderWidget] releaseGLResources() already released, returning";
        return;
    }

    if (!m_glInitialized)
    {
        qDebug() << "[RenderWidget] releaseGLResources() GL not initialized, marking released";
        m_glResourcesReleased = true;
        return;
    }

    QOpenGLContext* ctx = context();
    if (!ctx || !ctx->isValid())
    {
        qDebug() << "[RenderWidget] releaseGLResources() context invalid or null, marking released. ctx =" << ctx;
        m_glResourcesReleased = true;
        return;
    }

    qDebug() << "[RenderWidget] releaseGLResources() context valid, making current...";
    if (QOpenGLContext::currentContext() != ctx)
    {
        makeCurrent();
    }
    qDebug() << "[RenderWidget] releaseGLResources() makeCurrent done...";

    ShaderManager::instance().cleanup();

    // 着色器由 ShaderManager 统一管理，这里只清空指针
    m_lineProgram = nullptr;
    m_controlProgram = nullptr;
    m_crossProgram = nullptr;
    m_snapProgram = nullptr;
    m_flatProgram = nullptr;
    m_bitmapProgram = nullptr;

    if (m_lineVao)      { m_glFuncs->glDeleteVertexArrays(1, &m_lineVao);      m_lineVao = 0; }
    if (m_lineVbo)      { m_glFuncs->glDeleteBuffers(1, &m_lineVbo);           m_lineVbo = 0; }
    if (m_controlVao)   { m_glFuncs->glDeleteVertexArrays(1, &m_controlVao);   m_controlVao = 0; }
    if (m_controlVbo)   { m_glFuncs->glDeleteBuffers(1, &m_controlVbo);        m_controlVbo = 0; }
    if (m_pointMarkerVao) { m_glFuncs->glDeleteVertexArrays(1, &m_pointMarkerVao); m_pointMarkerVao = 0; }
    if (m_pointMarkerVbo) { m_glFuncs->glDeleteBuffers(1, &m_pointMarkerVbo);     m_pointMarkerVbo = 0; }
    if (m_crossVao)     { m_glFuncs->glDeleteVertexArrays(1, &m_crossVao);     m_crossVao = 0; }
    if (m_crossVbo)     { m_glFuncs->glDeleteBuffers(1, &m_crossVbo);          m_crossVbo = 0; }
    if (m_snapVao)      { m_glFuncs->glDeleteVertexArrays(1, &m_snapVao);      m_snapVao = 0; }
    if (m_snapVbo)      { m_glFuncs->glDeleteBuffers(1, &m_snapVbo);           m_snapVbo = 0; }
    if (m_flatVao)      { m_glFuncs->glDeleteVertexArrays(1, &m_flatVao);      m_flatVao = 0; }
    if (m_flatVbo)      { m_glFuncs->glDeleteBuffers(1, &m_flatVbo);           m_flatVbo = 0; }
    if (m_bitmapVao)    { m_glFuncs->glDeleteVertexArrays(1, &m_bitmapVao);    m_bitmapVao = 0; }
    if (m_bitmapVbo)    { m_glFuncs->glDeleteBuffers(1, &m_bitmapVbo);         m_bitmapVbo = 0; }
    if (m_bitmapTexture) { m_glFuncs->glDeleteTextures(1, &m_bitmapTexture);    m_bitmapTexture = 0; }
    if (m_selectionBoxVao)   { m_glFuncs->glDeleteVertexArrays(1, &m_selectionBoxVao);   m_selectionBoxVao = 0; }
    if (m_selectionBoxVbo)   { m_glFuncs->glDeleteBuffers(1, &m_selectionBoxVbo);        m_selectionBoxVbo = 0; }
    if (m_selectionHandleVao) { m_glFuncs->glDeleteVertexArrays(1, &m_selectionHandleVao); m_selectionHandleVao = 0; }
    if (m_selectionHandleVbo) { m_glFuncs->glDeleteBuffers(1, &m_selectionHandleVbo);     m_selectionHandleVbo = 0; }

    m_sceneConsumer.cleanup();

    delete m_glFuncs;
    m_glFuncs = nullptr;

    m_glResourcesReleased = true;
    qDebug() << "[RenderWidget] releaseGLResources() EXIT (context kept current), all GL resources released";
}

void RenderWidget::initializeGL()
{
    qDebug() << "[RenderWidget] initializeGL() called, size =" << size()
        << "isValid =" << isValid()
        << "context =" << context();

    m_glFuncs = new XGLFunctions();
    if (!m_glFuncs->initializeOpenGLFunctions())
    {
        SY_CRITICALF("[RenderWidget] Could not initialize OpenGL %d.%d functions",
            TARGET_GL_VERSION_MAJOR, TARGET_GL_VERSION_MINOR);
        qFatal("Could not initialize OpenGL functions");
    }

    SY_INFOF("[RenderWidget] OpenGL %s, GLSL %s",
        reinterpret_cast<const char*>(m_glFuncs->glGetString(GL_VERSION)),
        reinterpret_cast<const char*>(m_glFuncs->glGetString(GL_SHADING_LANGUAGE_VERSION)));

    // 初始化统一着色器管理器（必须在所有着色器使用之前）
    ShaderManager::instance().initialize(this);

    // 从 ShaderManager 获取所有着色器（非拥有指针）
    m_lineProgram    = ShaderManager::instance().get("line");
    m_controlProgram = ShaderManager::instance().get("control");
    m_crossProgram   = ShaderManager::instance().get("cross");
    m_snapProgram    = ShaderManager::instance().get("snap");
    m_flatProgram    = ShaderManager::instance().get("flat");
    m_bitmapProgram  = ShaderManager::instance().get("bitmap");

    // 预览线段 VAO/VBO
    m_glFuncs->glGenVertexArrays(1, &m_lineVao);
    m_glFuncs->glGenBuffers(1, &m_lineVbo);
    m_glFuncs->glBindVertexArray(m_lineVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);

    // 控制点辅助线 VAO/VBO
    m_glFuncs->glGenVertexArrays(1, &m_controlVao);
    m_glFuncs->glGenBuffers(1, &m_controlVbo);
    m_glFuncs->glBindVertexArray(m_controlVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_controlVbo);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);

    // 十字光标 VAO/VBO
    m_glFuncs->glGenVertexArrays(1, &m_crossVao);
    m_glFuncs->glGenBuffers(1, &m_crossVbo);
    m_glFuncs->glBindVertexArray(m_crossVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_crossVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, m_crossPoints.size() * sizeof(Render::Vec2f),
        m_crossPoints.data(), GL_STATIC_DRAW);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);

    // 捕捉标记 VAO/VBO
    m_glFuncs->glGenVertexArrays(1, &m_snapVao);
    m_glFuncs->glGenBuffers(1, &m_snapVbo);
    updateSnapGeometry();

    // 场景环境几何 VAO/VBO
    m_glFuncs->glGenVertexArrays(1, &m_flatVao);
    m_glFuncs->glGenBuffers(1, &m_flatVbo);

    // 持久化选择框 VAO/VBO（避免每帧创建/销毁）
    m_glFuncs->glGenVertexArrays(1, &m_selectionBoxVao);
    m_glFuncs->glGenBuffers(1, &m_selectionBoxVbo);
    m_glFuncs->glBindVertexArray(m_selectionBoxVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_selectionBoxVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, 8 * sizeof(Render::Vec2f), nullptr, GL_DYNAMIC_DRAW);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);

    // 持久化选择手柄 VAO/VBO（避免每帧创建/销毁）
    m_glFuncs->glGenVertexArrays(1, &m_selectionHandleVao);
    m_glFuncs->glGenBuffers(1, &m_selectionHandleVbo);
    m_glFuncs->glBindVertexArray(m_selectionHandleVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_selectionHandleVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, 4096 * sizeof(Render::Vec2f), nullptr, GL_DYNAMIC_DRAW);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);

    // 场景渲染
    m_sceneConsumer.initialize();
    m_sceneConsumer.setViewMatrix(m_viewMatrix);

    // 位图渲染
    initBitmapPipeline();

    m_glInitialized = true;
}

void RenderWidget::resizeGL(int w, int h)
{
    qDebug() << "[RenderWidget] resizeGL called, size =" << w << "x" << h;
    m_glFuncs->glViewport(0, 0, w, h);
    updateBitmapQuad();
}

void RenderWidget::paintGL()
{
    m_glFuncs->glClearColor(235.0f / 255.0f, 235.0f / 255.0f, 235.0f / 255.0f, 1.0f);
    m_glFuncs->glClear(GL_COLOR_BUFFER_BIT);

    m_glFuncs->glEnable(GL_BLEND);
    m_glFuncs->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    // 1. 场景环境几何（台面/网格/标尺）—— Engine 计算，Render 直接绘制
    renderSceneEnvGeo();

    // 位图（如果有）
    if (m_bBitmap && m_bitmapProgram && m_bitmapTexture)
    {
        m_glFuncs->glActiveTexture(GL_TEXTURE0);
        m_glFuncs->glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);

        if (m_bitmapProgram->bind())
        {
            const GLint projectionViewLoc = m_bitmapProgram->uniformLocation("projectionView");
            m_glFuncs->glUniformMatrix3fv(projectionViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
            const GLint texLoc = m_bitmapProgram->uniformLocation("uTex");
            m_glFuncs->glUniform1i(texLoc, 0);

            m_glFuncs->glBindVertexArray(m_bitmapVao);
            m_glFuncs->glDrawArrays(GL_TRIANGLES, 0, 6);
            m_glFuncs->glBindVertexArray(0);

            m_bitmapProgram->release();
        }

        m_glFuncs->glBindTexture(GL_TEXTURE_2D, 0);
    }

    // 场景图元
    if (m_bSceneData)
    {
        m_sceneConsumer.render(m_sceneCommands);
    }

    // 预览线
    if (m_linePoints.size() >= 2)
    {
        if (!m_lineProgram->bind())
        {
            static bool s_loggedOnce = false;
            if (!s_loggedOnce)
            {
                qWarning() << "[RenderWidget] m_lineProgram->bind() FAILED";
                s_loggedOnce = true;
            }
        }
        else
        {
            static bool s_loggedOnce = false;
            if (!s_loggedOnce)
            {
                qDebug() << "[RenderWidget] Preview rendering OK, linePoints =" << m_linePoints.size()
                    << "drawingColor =" << m_drawingColor;
                s_loggedOnce = true;
            }

            const GLint projectionViewLoc = m_lineProgram->uniformLocation("projectionView");
            const GLint colorLoc = m_lineProgram->uniformLocation("color");

            m_glFuncs->glUniformMatrix3fv(projectionViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
            m_glFuncs->glUniform4f(colorLoc,
                m_drawingColor.redF(), m_drawingColor.greenF(),
                m_drawingColor.blueF(), m_drawingColor.alphaF());

            m_glFuncs->glBindVertexArray(m_lineVao);
            if (m_linePoints.size() == 4)
            {
                m_glFuncs->glDrawArrays(GL_LINES, 0, 2);
                m_glFuncs->glDrawArrays(GL_LINES, 2, 2);
            }
            else
            {
                m_glFuncs->glDrawArrays(GL_LINE_STRIP, 0, static_cast<GLsizei>(m_linePoints.size()));
            }
            m_glFuncs->glBindVertexArray(0);
            m_lineProgram->release();
        }
    }

    // 控制点辅助线
    if (m_ctrlLines.size() >= 2 && m_controlProgram && m_controlProgram->bind())
    {
        const GLint projectionViewLoc = m_controlProgram->uniformLocation("projectionView");
        const GLint colorLoc = m_controlProgram->uniformLocation("color");

        m_glFuncs->glUniformMatrix3fv(projectionViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_ctrlLineColor.redF()),
            static_cast<float>(m_ctrlLineColor.greenF()),
            static_cast<float>(m_ctrlLineColor.blueF()),
            static_cast<float>(m_ctrlLineColor.alphaF()));

        m_glFuncs->glBindVertexArray(m_controlVao);
        m_glFuncs->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_ctrlLines.size()));
        m_glFuncs->glBindVertexArray(0);
        m_controlProgram->release();
    }

    // 屏幕空间点标记（选择手柄）
    if (!m_pointMarkers.empty() && m_controlProgram && m_controlProgram->bind())
    {
        float vpW = static_cast<float>(width());
        float vpH = static_cast<float>(height());
        if (vpW <= 1.0f)
            vpW = 1.0f;
        if (vpH <= 1.0f)
            vpH = 1.0f;

        float scaleX = std::abs(m_viewMatrix.at(0, 0));
        float scaleY = std::abs(m_viewMatrix.at(1, 1));
        if (scaleX < 1e-6f) scaleX = 1.0f;
        if (scaleY < 1e-6f) scaleY = 1.0f;

        float worldHalfW = m_pointMarkerSize / (scaleX * vpW);
        float worldHalfH = m_pointMarkerSize / (scaleY * vpH);

        std::vector<Render::Vec2f> verts;
        verts.reserve(m_pointMarkers.size() * 6);
        for (const auto& pm : m_pointMarkers)
        {
            float x0 = pm.worldPos.x() - worldHalfW;
            float x1 = pm.worldPos.x() + worldHalfW;
            float y0 = pm.worldPos.y() - worldHalfH;
            float y1 = pm.worldPos.y() + worldHalfH;
            verts.emplace_back(x0, y0); verts.emplace_back(x1, y0); verts.emplace_back(x1, y1);
            verts.emplace_back(x0, y0); verts.emplace_back(x1, y1); verts.emplace_back(x0, y1);
        }

        std::vector<Render::Vec2f> borders;
        borders.reserve(m_pointMarkers.size() * 8);
        for (const auto& pm : m_pointMarkers)
        {
            float x0 = pm.worldPos.x() - worldHalfW;
            float x1 = pm.worldPos.x() + worldHalfW;
            float y0 = pm.worldPos.y() - worldHalfH;
            float y1 = pm.worldPos.y() + worldHalfH;
            borders.emplace_back(x0, y0); borders.emplace_back(x1, y0);
            borders.emplace_back(x1, y0); borders.emplace_back(x1, y1);
            borders.emplace_back(x1, y1); borders.emplace_back(x0, y1);
            borders.emplace_back(x0, y1); borders.emplace_back(x0, y0);
        }

        if (m_pointMarkerVao == 0)
        {
            m_glFuncs->glGenVertexArrays(1, &m_pointMarkerVao);
            m_glFuncs->glGenBuffers(1, &m_pointMarkerVbo);
        }
        m_glFuncs->glBindVertexArray(m_pointMarkerVao);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_pointMarkerVbo);

        const GLint projectionViewLoc = m_controlProgram->uniformLocation("projectionView");
        const GLint colorLoc = m_controlProgram->uniformLocation("color");
        m_glFuncs->glUniformMatrix3fv(projectionViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);

        m_glFuncs->glBufferData(GL_ARRAY_BUFFER,
            verts.size() * sizeof(Render::Vec2f), verts.data(), GL_DYNAMIC_DRAW);
        m_glFuncs->glEnableVertexAttribArray(0);
        m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_pointMarkerFill.redF()),
            static_cast<float>(m_pointMarkerFill.greenF()),
            static_cast<float>(m_pointMarkerFill.blueF()),
            static_cast<float>(m_pointMarkerFill.alphaF()));
        m_glFuncs->glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));

        m_glFuncs->glBufferData(GL_ARRAY_BUFFER,
            borders.size() * sizeof(Render::Vec2f), borders.data(), GL_DYNAMIC_DRAW);
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_pointMarkerBorder.redF()),
            static_cast<float>(m_pointMarkerBorder.greenF()),
            static_cast<float>(m_pointMarkerBorder.blueF()),
            static_cast<float>(m_pointMarkerBorder.alphaF()));
        m_glFuncs->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(borders.size()));

        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glFuncs->glBindVertexArray(0);
        m_controlProgram->release();
    }

    // 选择框（持久化 VAO/VBO，避免每帧创建/销毁）
    if (m_hasSelectionBox && m_selectionBox.isValid()
        && m_controlProgram && m_controlProgram->bind())
    {
        float x0 = static_cast<float>(m_selectionBox.minPt.x());
        float y0 = static_cast<float>(m_selectionBox.minPt.y());
        float x1 = static_cast<float>(m_selectionBox.maxPt.x());
        float y1 = static_cast<float>(m_selectionBox.maxPt.y());
        Render::Vec2f lines[] = {
            Render::Vec2f(x0, y0), Render::Vec2f(x1, y0),
            Render::Vec2f(x1, y0), Render::Vec2f(x1, y1),
            Render::Vec2f(x1, y1), Render::Vec2f(x0, y1),
            Render::Vec2f(x0, y1), Render::Vec2f(x0, y0),
        };

        const GLint projViewLoc = m_controlProgram->uniformLocation("projectionView");
        const GLint colorLoc = m_controlProgram->uniformLocation("color");
        m_glFuncs->glUniformMatrix3fv(projViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_selectionBoxColor.redF()),
            static_cast<float>(m_selectionBoxColor.greenF()),
            static_cast<float>(m_selectionBoxColor.blueF()),
            static_cast<float>(m_selectionBoxColor.alphaF()));

        m_glFuncs->glBindVertexArray(m_selectionBoxVao);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_selectionBoxVbo);
        m_glFuncs->glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(lines), lines);
        m_glFuncs->glDrawArrays(GL_LINES, 0, 8);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glFuncs->glBindVertexArray(0);

        m_controlProgram->release();
    }

    // 选择手柄（持久化 VAO/VBO，避免每帧创建/销毁）
    if (!m_selectionHandles.empty() && m_controlProgram && m_controlProgram->bind())
    {
        float vpW = static_cast<float>(width());
        float vpH = static_cast<float>(height());
        if (vpW <= 1.0f) vpW = 1.0f;
        if (vpH <= 1.0f) vpH = 1.0f;
        float scaleX = std::abs(m_viewMatrix.at(0, 0));
        float scaleY = std::abs(m_viewMatrix.at(1, 1));
        if (scaleX < 1e-6f) scaleX = 1.0f;
        if (scaleY < 1e-6f) scaleY = 1.0f;

        float worldHalfW = m_selectionHandleSize / (scaleX * vpW);
        float worldHalfH = m_selectionHandleSize / (scaleY * vpH);

        std::vector<Render::Vec2f> verts;
        verts.reserve(m_selectionHandles.size() * 6);
        for (const auto& pm : m_selectionHandles)
        {
            float x0 = pm.worldPos.x() - worldHalfW;
            float x1 = pm.worldPos.x() + worldHalfW;
            float y0 = pm.worldPos.y() - worldHalfH;
            float y1 = pm.worldPos.y() + worldHalfH;
            verts.emplace_back(x0, y0); verts.emplace_back(x1, y0); verts.emplace_back(x1, y1);
            verts.emplace_back(x0, y0); verts.emplace_back(x1, y1); verts.emplace_back(x0, y1);
        }

        std::vector<Render::Vec2f> borders;
        borders.reserve(m_selectionHandles.size() * 8);
        for (const auto& pm : m_selectionHandles)
        {
            float x0 = pm.worldPos.x() - worldHalfW;
            float x1 = pm.worldPos.x() + worldHalfW;
            float y0 = pm.worldPos.y() - worldHalfH;
            float y1 = pm.worldPos.y() + worldHalfH;
            borders.emplace_back(x0, y0); borders.emplace_back(x1, y0);
            borders.emplace_back(x1, y0); borders.emplace_back(x1, y1);
            borders.emplace_back(x1, y1); borders.emplace_back(x0, y1);
            borders.emplace_back(x0, y1); borders.emplace_back(x0, y0);
        }

        const GLint projViewLoc = m_controlProgram->uniformLocation("projectionView");
        const GLint colorLoc = m_controlProgram->uniformLocation("color");
        m_glFuncs->glUniformMatrix3fv(projViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);

        m_glFuncs->glBindVertexArray(m_selectionHandleVao);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_selectionHandleVbo);

        // 确保 VBO 容量足够
        size_t totalVerts = verts.size() + borders.size();
        if (totalVerts * sizeof(Render::Vec2f) > 4096 * sizeof(Render::Vec2f))
        {
            m_glFuncs->glBufferData(GL_ARRAY_BUFFER, totalVerts * sizeof(Render::Vec2f), nullptr, GL_DYNAMIC_DRAW);
        }

        // 填充面
        m_glFuncs->glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(Render::Vec2f), verts.data());
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_selectionHandleFill.redF()),
            static_cast<float>(m_selectionHandleFill.greenF()),
            static_cast<float>(m_selectionHandleFill.blueF()),
            static_cast<float>(m_selectionHandleFill.alphaF()));
        m_glFuncs->glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size()));

        // 边框线
        m_glFuncs->glBufferSubData(GL_ARRAY_BUFFER, 0, borders.size() * sizeof(Render::Vec2f), borders.data());
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_selectionHandleBorder.redF()),
            static_cast<float>(m_selectionHandleBorder.greenF()),
            static_cast<float>(m_selectionHandleBorder.blueF()),
            static_cast<float>(m_selectionHandleBorder.alphaF()));
        m_glFuncs->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(borders.size()));

        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glFuncs->glBindVertexArray(0);

        m_controlProgram->release();
    }

    // 十字光标
    if (m_crossPoints.size() >= 2 && m_crossProgram->bind())
    {
        const GLint projectionViewLoc = m_crossProgram->uniformLocation("projectionView");
        m_glFuncs->glUniformMatrix3fv(projectionViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
        const GLint mouseWorldPosLoc = m_crossProgram->uniformLocation("mousePos");
        m_glFuncs->glUniform2f(mouseWorldPosLoc, m_mouseWorldPos.x(), m_mouseWorldPos.y());
        const GLint viewportSizeLoc = m_crossProgram->uniformLocation("viewportSize");
        m_glFuncs->glUniform2f(viewportSizeLoc, static_cast<float>(width()), static_cast<float>(height()));

        m_glFuncs->glBindVertexArray(m_crossVao);
        m_glFuncs->glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_crossPoints.size()));
        m_glFuncs->glBindVertexArray(0);
        m_crossProgram->release();
    }

    // 捕捉标记
    if (m_bSnapIndicator && m_snapProgram && m_snapProgram->bind())
    {
        const GLint projViewLoc = m_snapProgram->uniformLocation("projectionView");
        const GLint snapPosLoc = m_snapProgram->uniformLocation("snapWorldPos");
        const GLint vpSizeLoc = m_snapProgram->uniformLocation("viewportSize");
        const GLint markerSizeLoc = m_snapProgram->uniformLocation("markerSize");
        const GLint colorLoc = m_snapProgram->uniformLocation("markerColor");

        m_glFuncs->glUniformMatrix3fv(projViewLoc, 1, GL_FALSE, &m_viewMatrix.data[0]);
        m_glFuncs->glUniform2f(snapPosLoc,
            static_cast<float>(m_snapWorldPos.x()),
            static_cast<float>(m_snapWorldPos.y()));
        m_glFuncs->glUniform2f(vpSizeLoc,
            static_cast<float>(width()), static_cast<float>(height()));
        m_glFuncs->glUniform1f(markerSizeLoc, 10.0f);
        m_glFuncs->glUniform4f(colorLoc,
            static_cast<float>(m_snapColor.redF()),
            static_cast<float>(m_snapColor.greenF()),
            static_cast<float>(m_snapColor.blueF()),
            1.0f);

        m_glFuncs->glBindVertexArray(m_snapVao);
        m_glFuncs->glDrawArrays(GL_LINE_LOOP, 0, 4);
        m_glFuncs->glDrawArrays(GL_LINES, 4, 4);
        m_glFuncs->glBindVertexArray(0);
        m_snapProgram->release();
    }

    m_glFuncs->glDisable(GL_BLEND);
}

void RenderWidget::paintEvent(QPaintEvent* event)
{
    // 1. 先调用 OpenGL 渲染（几何/网格/标尺线条等）
    QOpenGLWidget::paintEvent(event);

    // 2. 再用 QPainter 叠加绘制所有 UI 文字（标尺刻度数字、通用 UI 文字、坐标提示）
    paintUiTexts();
}

void RenderWidget::showEvent(QShowEvent* event)
{
    qDebug() << "[RenderWidget] showEvent called, size =" << size();
    QOpenGLWidget::showEvent(event);
}

// ===== UI 文字绘制（QPainter）=====

void RenderWidget::paintUiTexts()
{
    const int vpW = width();
    const int vpH = height();
    if (vpW <= 0 || vpH <= 0) return;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    // 收集所有待绘制文字（标尺文字 + 通用 UI 文字），按 zOrder 排序
    std::vector<const Render::UiTextItem*> allItems;
    allItems.reserve(m_sceneEnvGeo.texts.size() + m_uiTexts.size());

    for (const auto& t : m_sceneEnvGeo.texts)
        allItems.push_back(&t);
    for (const auto& t : m_uiTexts)
        allItems.push_back(&t);

    // 如果启用了坐标提示，动态添加一个
    std::unique_ptr<Render::UiTextItem> coordItem;
    if (m_bShowMouseCoord)
    {
        coordItem = std::make_unique<Render::UiTextItem>();
        coordItem->text =
            "X: " + std::to_string(m_mouseWorldPos.x()).substr(0,
                std::to_string(m_mouseWorldPos.x()).find('.') + 3) +
            "   Y: " + std::to_string(m_mouseWorldPos.y()).substr(0,
                std::to_string(m_mouseWorldPos.y()).find('.') + 3);
        coordItem->x = static_cast<float>(vpW - 10);
        coordItem->y = static_cast<float>(vpH - 10);
        coordItem->coordMode = Render::UiTextCoordMode::PixelCoords;
        coordItem->hAlign = Render::UiTextHAlign::Right;
        coordItem->vAlign = Render::UiTextVAlign::Bottom;
        coordItem->fontSize = 11;
        coordItem->color = Render::Color::fromRGB255(
            m_mouseCoordColor.red(), m_mouseCoordColor.green(),
            m_mouseCoordColor.blue(), m_mouseCoordColor.alpha());
        coordItem->zOrder = 100.0f;
        coordItem->hasBackground = true;
        coordItem->bgColor = Render::Color::fromRGB255(255, 255, 255, 220);
        coordItem->bgPaddingX = 6.0f;
        coordItem->bgPaddingY = 3.0f;
        coordItem->bgRadius = 3.0f;
        allItems.push_back(coordItem.get());
    }

    // 按 zOrder 排序（小的先画，大的在上面）
    std::sort(allItems.begin(), allItems.end(),
        [](const Render::UiTextItem* a, const Render::UiTextItem* b) {
            return a->zOrder < b->zOrder;
        });

    // 世界坐标 → 像素坐标辅助（屏幕中心为原点，Y 向上的约定）
    // 我们这里使用 viewMatrix 把 world (x,y) 变换到 NDC (-1~1)，再换算到像素坐标
    auto worldToPixel = [&](float wx, float wy) -> QPointF {
        float nx = m_viewMatrix.at(0, 0) * wx + m_viewMatrix.at(0, 1) * wy + m_viewMatrix.at(0, 2);
        float ny = m_viewMatrix.at(1, 0) * wx + m_viewMatrix.at(1, 1) * wy + m_viewMatrix.at(1, 2);
        float px = (nx + 1.0f) * 0.5f * static_cast<float>(vpW);
        float py = (1.0f - ny) * 0.5f * static_cast<float>(vpH);
        return QPointF(px, py);
        };

    // 绘制每一项
    for (const Render::UiTextItem* item : allItems)
    {
        if (!item || item->text.empty()) continue;

        // 计算锚点（像素坐标）
        QPointF anchorPx;
        if (item->coordMode == Render::UiTextCoordMode::PixelCoords)
        {
            anchorPx = QPointF(item->x, item->y);
        }
        else
        {
            anchorPx = worldToPixel(item->x, item->y);
        }

        // 视口裁剪：文字锚点在视口外太大距离就跳过，避免无意义绘制
        const float margin = 200.0f;
        if (anchorPx.x() < -margin || anchorPx.x() > float(vpW) + margin ||
            anchorPx.y() < -margin || anchorPx.y() > float(vpH) + margin)
        {
            continue;
        }

        // 字体（使用点大小，确保与全局字体设置一致）
        QFont font = painter.font();
        font.setPointSize(std::max(8, int(item->fontSize)));
        painter.setFont(font);

        // 文字颜色
        QColor textColor;
        if (item->color.a() > 0.0f)
        {
            textColor = QColor::fromRgbF(
                item->color.r(), item->color.g(),
                item->color.b(), item->color.a());
        }
        else
        {
            textColor = QColor(30, 30, 30);
        }
        painter.setPen(textColor);

        // 测量文字尺寸
        QFontMetrics fm(font);
        QString qText = QString::fromStdString(item->text);
        int textW = fm.horizontalAdvance(qText);
        int textH = fm.height();
        int textAscent = fm.ascent();

        // 计算对齐后的文字框四角（像素坐标，Qt 窗口坐标）
        float boxLeft = 0.0f;
        float boxTop = 0.0f;
        float boxRight = 0.0f;
        float boxBottom = 0.0f;

        // 水平对齐
        switch (item->hAlign)
        {
            case Render::UiTextHAlign::Left:
                boxLeft = anchorPx.x();
                break;
            case Render::UiTextHAlign::Center:
                boxLeft = anchorPx.x() - static_cast<float>(textW) * 0.5f;
                break;
            case Render::UiTextHAlign::Right:
                boxLeft = anchorPx.x() - static_cast<float>(textW);
                break;
        }
        boxRight = boxLeft + static_cast<float>(textW);

        // 垂直对齐（文字框高 = textH）
        switch (item->vAlign)
        {
            case Render::UiTextVAlign::Top:
                boxTop = anchorPx.y();
                break;
            case Render::UiTextVAlign::Middle:
                boxTop = anchorPx.y() - static_cast<float>(textH) * 0.5f;
                break;
            case Render::UiTextVAlign::Bottom:
                boxTop = anchorPx.y() - static_cast<float>(textH);
                break;
        }
        boxBottom = boxTop + static_cast<float>(textH);

        // 文字绘制原点（Qt drawText(x,y,...) 中 y 是基线位置）
        float textBaselineX = boxLeft;
        float textBaselineY = boxTop + static_cast<float>(textAscent);

        // 可选指示线（用于世界锚点的标记）
        if (item->hasLeaderLine && item->coordMode == Render::UiTextCoordMode::WorldPos_PixelSize)
        {
            QPen leaderPen(QColor::fromRgbF(
                item->leaderLineColor.r(), item->leaderLineColor.g(),
                item->leaderLineColor.b(), item->leaderLineColor.a()));
            leaderPen.setWidthF(item->leaderLineWidth > 0 ? item->leaderLineWidth : 1.0f);
            painter.setPen(leaderPen);

            QPointF worldPx = worldToPixel(item->x, item->y);
            QPointF boxCenter(
                boxLeft + (boxRight - boxLeft) * 0.5f,
                boxTop + (boxBottom - boxTop) * 0.5f);
            painter.drawLine(worldPx, boxCenter);

            painter.setPen(textColor);
        }

        // 可选背景（外扩 padding）
        if (item->hasBackground)
        {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QBrush(QColor::fromRgbF(
                item->bgColor.r(), item->bgColor.g(),
                item->bgColor.b(), item->bgColor.a())));

            QRectF bgRect(
                boxLeft - item->bgPaddingX,
                boxTop - item->bgPaddingY,
                static_cast<float>(textW) + 2.0f * item->bgPaddingX,
                static_cast<float>(textH) + 2.0f * item->bgPaddingY);

            if (item->bgRadius > 0.0f)
            {
                painter.drawRoundedRect(bgRect, item->bgRadius, item->bgRadius);
            }
            else
            {
                painter.drawRect(bgRect);
            }
            painter.setBrush(Qt::NoBrush);
            painter.setPen(textColor);
        }

        // 处理旋转（仅用于世界定位文字，视觉旋转不影响屏幕文字对齐计算）
        const bool needRotate = (item->rotationDeg != 0.0f &&
            item->coordMode == Render::UiTextCoordMode::WorldPos_PixelSize);

        if (needRotate)
        {
            painter.save();
            painter.translate(anchorPx);
            painter.rotate(item->rotationDeg);
            painter.translate(-anchorPx);
        }

        // 绘制文字（基线对齐）
        painter.drawText(QPointF(textBaselineX, textBaselineY), qText);

        if (needRotate)
        {
            painter.restore();
        }
    }
}

// ===== 鼠标事件 =====

void RenderWidget::onMousePressEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton)
    {
        updateLineBuffer();
        update();
    }
    else if (event->button() == Qt::MiddleButton)
    {
        m_lastPos = event->pos();
    }
}

void RenderWidget::onMouseMoveEvent(QMouseEvent* event)
{
    // 更新鼠标位置
    m_mousePos = event->pos();
    m_bMouseTracking = true;

    // 更新十字光标位置
    updateCrossBuffer();
    update();
}

void RenderWidget::onMouseReleaseEvent(QMouseEvent* event)
{
    (void)event;
}

void RenderWidget::onWheelEvent(QWheelEvent* event)
{
    (void)event;
}

void RenderWidget::setMouseWorldPos(const QPointF& worldPos)
{
    m_mouseWorldPos = worldPos;
    update();
}

bool RenderWidget::setBitmapRGBA(const unsigned char* rgba, int w, int h,
    float tlX, float tlY, float trX, float trY,
    float blX, float blY, float brX, float brY)
{
    if (!rgba || w <= 0 || h <= 0)
        return false;

    if (!m_glFuncs)
        return false;

    makeCurrent();

    if (!m_bitmapTexture)
        m_glFuncs->glGenTextures(1, &m_bitmapTexture);

    m_glFuncs->glBindTexture(GL_TEXTURE_2D, m_bitmapTexture);
    m_glFuncs->glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
#ifdef GL_UNPACK_FLIP_Y_WEBGL
    m_glFuncs->glPixelStorei(GL_UNPACK_FLIP_Y_WEBGL, true);
#else
#endif
    m_glFuncs->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    m_glFuncs->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_glFuncs->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    m_glFuncs->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    m_glFuncs->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    m_glFuncs->glBindTexture(GL_TEXTURE_2D, 0);

    doneCurrent();

    m_bitmapW = w;
    m_bitmapH = h;
    m_bitmapTlX = tlX; m_bitmapTlY = tlY;
    m_bitmapTrX = trX; m_bitmapTrY = trY;
    m_bitmapBlX = blX; m_bitmapBlY = blY;
    m_bitmapBrX = brX; m_bitmapBrY = brY;

    m_bBitmap = true;

    updateBitmapQuad();
    update();
    return true;
}

void RenderWidget::setBitmapPosition(float tlX, float tlY, float trX, float trY,
    float blX, float blY, float brX, float brY)
{
    if (!m_bBitmap || !m_bitmapVbo)
        return;

    m_bitmapTlX = tlX; m_bitmapTlY = tlY;
    m_bitmapTrX = trX; m_bitmapTrY = trY;
    m_bitmapBlX = blX; m_bitmapBlY = blY;
    m_bitmapBrX = brX; m_bitmapBrY = brY;

    updateBitmapQuad();
    update();
}

void RenderWidget::clearBitmap()
{
    if (!m_bBitmap && !m_bitmapTexture)
        return;

    if (!m_glFuncs)
    {
        m_bBitmap = false;
        m_bitmapW = 0;
        m_bitmapH = 0;
        m_bitmapTlX = 0.0f; m_bitmapTlY = 0.0f;
        m_bitmapTrX = 0.0f; m_bitmapTrY = 0.0f;
        m_bitmapBlX = 0.0f; m_bitmapBlY = 0.0f;
        m_bitmapBrX = 0.0f; m_bitmapBrY = 0.0f;
        update();
        return;
    }

    makeCurrent();
    if (m_bitmapTexture)
    {
        m_glFuncs->glDeleteTextures(1, &m_bitmapTexture);
        m_bitmapTexture = 0;
    }
    doneCurrent();

    m_bBitmap = false;
    m_bitmapW = 0;
    m_bitmapH = 0;
    m_bitmapTlX = 0.0f; m_bitmapTlY = 0.0f;
    m_bitmapTrX = 0.0f; m_bitmapTrY = 0.0f;
    m_bitmapBlX = 0.0f; m_bitmapBlY = 0.0f;
    m_bitmapBrX = 0.0f; m_bitmapBrY = 0.0f;
    update();
}

void RenderWidget::updateLineBuffer()
{
    if (!m_glFuncs)
        return;

    makeCurrent();
    m_glFuncs->glBindVertexArray(m_lineVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_lineVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, m_linePoints.size() * sizeof(Render::Vec2f),
        m_linePoints.data(), GL_STATIC_DRAW);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Render::Vec2f), nullptr);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);
    doneCurrent();
}

void RenderWidget::updateCrossBuffer()
{
    if (!m_glFuncs)
        return;

    if (!m_bMouseTracking)
    {
        m_mousePos = QPoint(width() / 2, height() / 2);
    }

    // 十字光标大小（固定像素大小，例如20像素）
    float crossSize = 20.0f;

    // 更新十字光标的坐标
    m_crossPoints.clear();
    m_crossPoints.push_back(Render::Vec2f(-crossSize, 0.0f));
    m_crossPoints.push_back(Render::Vec2f(crossSize, 0.0f));
    m_crossPoints.push_back(Render::Vec2f(0.0f, -crossSize));
    m_crossPoints.push_back(Render::Vec2f(0.0f, crossSize));

    makeCurrent();
    m_glFuncs->glBindVertexArray(m_crossVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_crossVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, m_crossPoints.size() * sizeof(Render::Vec2f),
        m_crossPoints.data(), GL_STATIC_DRAW);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Render::Vec2f), nullptr);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);
    doneCurrent();
}

void RenderWidget::initBitmapPipeline()
{
    // 位图着色器由 ShaderManager 统一管理
    m_bitmapProgram = ShaderManager::instance().bitmapShader();

    m_glFuncs->glGenVertexArrays(1, &m_bitmapVao);
    m_glFuncs->glGenBuffers(1, &m_bitmapVbo);
    m_glFuncs->glBindVertexArray(m_bitmapVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_bitmapVbo);

    float verts[] = {
        // pos      // uv
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f, -1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 1.f,
    };
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_DYNAMIC_DRAW);

    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(0));
    m_glFuncs->glEnableVertexAttribArray(1);
    m_glFuncs->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), reinterpret_cast<void*>(2 * sizeof(float)));

    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);
}

void RenderWidget::updateBitmapQuad()
{
    if (!m_glFuncs)
        return;

    if (!m_bitmapVbo)
        return;

    if (!m_bBitmap)
        return;

    // 使用四个角点直接构建四边形（支持旋转、斜切等任意仿射变换）
    // UV 映射：topLeft -> (0,0)，topRight -> (1,0)，bottomRight -> (1,1)，bottomLeft -> (0,1)
    // 由于纹理上传时 Y 轴翻转，这个映射使图像正确显示
    float verts[] = {
        m_bitmapBlX, m_bitmapBlY,  0.f, 1.f,
        m_bitmapBrX, m_bitmapBrY,  1.f, 1.f,
        m_bitmapTrX, m_bitmapTrY,  1.f, 0.f,
        m_bitmapBlX, m_bitmapBlY,  0.f, 1.f,
        m_bitmapTrX, m_bitmapTrY,  1.f, 0.f,
        m_bitmapTlX, m_bitmapTlY,  0.f, 0.f,
    };

    makeCurrent();
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_bitmapVbo);
    m_glFuncs->glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(verts), verts);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    doneCurrent();
}

void RenderWidget::setViewMatrix(const Ut::Mat3f& matrix)
{
    m_viewMatrix = matrix;
    m_sceneConsumer.setViewMatrix(matrix);
    update();
}

void RenderWidget::setDrawingColor(const QColor& color)
{
    m_drawingColor = color;
    update();
}

void RenderWidget::setAntialiasing()
{
    if (!m_glFuncs)
        return;

    makeCurrent();
    m_bAntialiasing = !m_bAntialiasing;
    if (m_bAntialiasing)
    {
        m_glFuncs->glEnable(GL_LINE_SMOOTH);
        m_glFuncs->glEnable(GL_BLEND);
        m_glFuncs->glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    }
    else
    {
        m_glFuncs->glDisable(GL_LINE_SMOOTH);
        m_glFuncs->glDisable(GL_BLEND);
    }
    doneCurrent();
    update();
}

void RenderWidget::setWireframeMode()
{
    if (!m_glFuncs)
        return;

    makeCurrent();
    m_bWireframeMode = !m_bWireframeMode;
    if (m_bWireframeMode)
    {
        m_glFuncs->glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    else
    {
        m_glFuncs->glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    doneCurrent();
    update();
}

void RenderWidget::setDepthTest()
{
    if (!m_glFuncs)
        return;

    makeCurrent();
    m_bDepthTest = !m_bDepthTest;
    if (m_bDepthTest)
    {
        m_glFuncs->glEnable(GL_DEPTH_TEST);
    }
    else
    {
        m_glFuncs->glDisable(GL_DEPTH_TEST);
    }
    doneCurrent();
    update();
}

void RenderWidget::resetView()
{
    m_scale = 1.0f;
    m_translation = QVector2D(0.0f, 0.0f);
    m_viewMatrix = Ut::Mat3f::identity();
    m_sceneConsumer.setViewMatrix(m_viewMatrix);
    update();
}

void RenderWidget::setSceneCommands(Render::RenderCommandList&& cmdList)
{
    m_sceneCommands = std::move(cmdList);
    m_bSceneData = !m_sceneCommands.commands.empty();
    update();
}

void RenderWidget::setPreviewPoints(const Render::Vec2f* points, size_t count)
{
    m_linePoints.clear();
    if (points && count > 0)
    {
        m_linePoints.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            m_linePoints.push_back(Render::Vec2f(points[i][0], points[i][1]));
        }
    }
    // qDebug() << "[RenderWidget] setPreviewPoints count =" << count;
    updateLineBuffer();
    update();
}

void RenderWidget::setControlLines(const Render::Vec2f* points, size_t count)
{
    m_ctrlLines.clear();
    if (points && count > 0)
    {
        m_ctrlLines.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            m_ctrlLines.push_back(Render::Vec2f(points[i][0], points[i][1]));
        }
    }

    if (!m_glFuncs)
    {
        update();
        return;
    }

    if (!m_ctrlLines.empty())
    {
        makeCurrent();
        m_glFuncs->glBindVertexArray(m_controlVao);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_controlVbo);
        m_glFuncs->glBufferData(GL_ARRAY_BUFFER, m_ctrlLines.size() * sizeof(Render::Vec2f),
            m_ctrlLines.data(), GL_STATIC_DRAW);
        m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Render::Vec2f), nullptr);
        m_glFuncs->glEnableVertexAttribArray(0);
        m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
        m_glFuncs->glBindVertexArray(0);
        doneCurrent();
    }

    update();
}

void RenderWidget::setControlLines(const Render::Vec2f* points, size_t count, const QColor& color)
{
    m_ctrlLineColor = color;
    setControlLines(points, count);
}

void RenderWidget::setPointMarkers(const Render::Vec2f* worldPoints, size_t count,
    float markerSize, const QColor& fillColor, const QColor& borderColor)
{
    m_pointMarkerSize = markerSize;
    m_pointMarkerFill = fillColor;
    m_pointMarkerBorder = borderColor;

    m_pointMarkers.clear();
    if (worldPoints && count > 0)
    {
        m_pointMarkers.reserve(count);
        for (size_t i = 0; i < count; ++i)
            m_pointMarkers.push_back({ Render::Vec2f(worldPoints[i][0], worldPoints[i][1]) });
    }

    update();
}

void RenderWidget::clearSelectionPreview()
{
    m_linePoints.clear();
    m_ctrlLines.clear();
    m_pointMarkers.clear();
    update();
}

void RenderWidget::setSelectionBox(const Render::BBox2d* bbox, const QColor& color)
{
    if (!bbox || !bbox->isValid())
    {
        m_hasSelectionBox = false;
    }
    else
    {
        m_hasSelectionBox = true;
        // 从 double-bbox 拷贝到 float 兼容的 BBox2d（内部数据类型仍为 double）
        m_selectionBox = *bbox;
        m_selectionBoxColor = color;
    }
    update();
}

void RenderWidget::setSelectionHandles(const Render::Vec2f* worldPoints, size_t count,
    float markerSize, const QColor& fillColor, const QColor& borderColor)
{
    m_selectionHandleSize = markerSize;
    m_selectionHandleFill = fillColor;
    m_selectionHandleBorder = borderColor;

    m_selectionHandles.clear();
    if (worldPoints && count > 0)
    {
        m_selectionHandles.reserve(count);
        for (size_t i = 0; i < count; ++i)
            m_selectionHandles.push_back({ Render::Vec2f(worldPoints[i][0], worldPoints[i][1]) });
    }
    update();
}

void RenderWidget::clearSelectionDecoration()
{
    m_hasSelectionBox = false;
    m_selectionHandles.clear();
    update();
}

void RenderWidget::setSnapIndicator(const QPointF& worldPos, bool visible)
{
    m_snapWorldPos = worldPos;
    m_bSnapIndicator = visible;
    update();
}

void RenderWidget::clearSnapIndicator()
{
    m_bSnapIndicator = false;
    update();
}

void RenderWidget::setSnapIndicatorColor(const QColor& color)
{
    m_snapColor = color;
    update();
}

void RenderWidget::updateSnapGeometry()
{
    if (!m_glFuncs)
        return;

    // 捕捉标记几何体：正方形 + 对角线X
    // 正方形顶点（归一化到 [-1, 1]，在着色器中按像素缩放）
    const float s = 1.0f;
    m_snapPoints = {
        Render::Vec2f(-s, -s), Render::Vec2f(s, -s), Render::Vec2f(s, s), Render::Vec2f(-s, s),  // 正方形 (4)
        Render::Vec2f(-s, -s), Render::Vec2f(s, s), Render::Vec2f(s, -s), Render::Vec2f(-s, s),  // X对角线 (4)
    };

    m_glFuncs->glBindVertexArray(m_snapVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_snapVbo);
    m_glFuncs->glBufferData(GL_ARRAY_BUFFER, m_snapPoints.size() * sizeof(Render::Vec2f),
        m_snapPoints.data(), GL_STATIC_DRAW);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Render::Vec2f), nullptr);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);
}

// ---------- 场景环境几何：Engine 计算 → Render 绘制 ----------

void RenderWidget::setSceneEnvGeometry(const Render::SceneEnvGeometry& geo)
{
    m_sceneEnvGeo = geo;
    update();
}

void RenderWidget::renderSceneEnvGeo()
{
    if (m_sceneEnvGeo.layers.empty() || !m_flatProgram) return;

    if (!m_flatProgram->bind()) return;
    m_glFuncs->glBindVertexArray(m_flatVao);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, m_flatVbo);
    m_glFuncs->glEnableVertexAttribArray(0);
    m_glFuncs->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

    // pixel -> NDC 矩阵（用于屏幕空间的标尺等）
    Ut::Mat3f pxToNdc;
    {
        float vpW = float(width());
        float vpH = float(height());
        if (vpW <= 0.0f) vpW = 1.0f;
        if (vpH <= 0.0f) vpH = 1.0f;
        pxToNdc = Ut::Mat3f::identity();
        pxToNdc.at(0, 0) = 2.0f / vpW;
        pxToNdc.at(1, 1) = -2.0f / vpH;
        pxToNdc.at(0, 2) = -1.0f;
        pxToNdc.at(1, 2) = 1.0f;
    }

    const GLint colorLoc = m_flatProgram->uniformLocation("color");
    const GLint zLoc = m_flatProgram->uniformLocation("zDepth");
    const GLint matLoc = m_flatProgram->uniformLocation("screenToNdc");

    for (const auto& layer : m_sceneEnvGeo.layers)
    {
        if (layer.vertices.empty()) continue;

        const Ut::Mat3f& mat = layer.usePixelCoords ? pxToNdc : m_viewMatrix;

        m_glFuncs->glUniform4f(colorLoc,
            layer.color.r(), layer.color.g(),
            layer.color.b(), layer.color.a());
        m_glFuncs->glUniform1f(zLoc, layer.zDepth);
        m_glFuncs->glUniformMatrix3fv(matLoc, 1, GL_FALSE, mat.data);

        m_glFuncs->glBufferData(GL_ARRAY_BUFFER,
            GLsizeiptr(layer.vertices.size() * sizeof(Render::Vec2f)),
            layer.vertices.data(), GL_DYNAMIC_DRAW);

        if (layer.asTriangles)
        {
            m_glFuncs->glDrawArrays(GL_TRIANGLES, 0, GLsizei(layer.vertices.size()));
        }
        else
        {
            m_glFuncs->glLineWidth(layer.lineWidth > 0 ? layer.lineWidth : 1.0f);
            m_glFuncs->glDrawArrays(GL_LINES, 0, GLsizei(layer.vertices.size()));
            m_glFuncs->glLineWidth(1.0f);
        }
    }

    m_glFuncs->glDisableVertexAttribArray(0);
    m_glFuncs->glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_glFuncs->glBindVertexArray(0);
    m_flatProgram->release();
}

// ===== UI 文字 API 实现 =====

void RenderWidget::setUiTexts(const Render::UiTextItemList& texts)
{
    m_uiTexts = texts;
    update();
}

void RenderWidget::setUiTexts(Render::UiTextItemList&& texts)
{
    m_uiTexts = std::move(texts);
    update();
}

void RenderWidget::addUiText(const Render::UiTextItem& text)
{
    m_uiTexts.push_back(text);
    update();
}

void RenderWidget::clearUiTexts()
{
    m_uiTexts.clear();
    update();
}

void RenderWidget::addScreenText(const std::string& text,
    float pixelX, float pixelY,
    int fontSize,
    const QColor& color,
    Render::UiTextHAlign hAlign,
    Render::UiTextVAlign vAlign)
{
    Render::UiTextItem item;
    item.text = text;
    item.x = pixelX;
    item.y = pixelY;
    item.coordMode = Render::UiTextCoordMode::PixelCoords;
    item.hAlign = hAlign;
    item.vAlign = vAlign;
    item.fontSize = fontSize;
    item.color = Render::Color::fromRGB255(
        color.red(), color.green(), color.blue(), color.alpha());
    item.zOrder = 50.0f;
    m_uiTexts.push_back(item);
    update();
}

void RenderWidget::addWorldAnchorText(const std::string& text,
    float worldX, float worldY, int fontSize,
    const QColor& color,
    Render::UiTextHAlign hAlign, Render::UiTextVAlign vAlign,
    float rotationDeg)
{
    Render::UiTextItem item;
    item.text = text;
    item.x = worldX;
    item.y = worldY;
    item.coordMode = Render::UiTextCoordMode::WorldPos_PixelSize;
    item.hAlign = hAlign;
    item.vAlign = vAlign;
    item.fontSize = fontSize;
    item.color = Render::Color::fromRGB255(
        color.red(), color.green(), color.blue(), color.alpha());
    item.rotationDeg = rotationDeg;
    item.zOrder = 20.0f;
    m_uiTexts.push_back(item);
    update();
}

void RenderWidget::setMouseCoordinateDisplay(bool on, const QColor& color)
{
    m_bShowMouseCoord = on;
    m_mouseCoordColor = color;
    update();
}

void RenderWidget::setMeasurementText(const std::string& text,
    float worldX, float worldY,
    int fontSize,
    const QColor& color)
{
    // 清除之前的测量文字
    m_uiTexts.erase(
        std::remove_if(m_uiTexts.begin(), m_uiTexts.end(),
            [](const Render::UiTextItem& item) {
                return item.zOrder >= 10.0f && item.zOrder < 20.0f;
            }),
        m_uiTexts.end());

    Render::UiTextItem item;
    item.text = text;
    item.x = worldX;
    item.y = worldY;
    item.coordMode = Render::UiTextCoordMode::WorldPos_PixelSize;
    item.hAlign = Render::UiTextHAlign::Center;
    item.vAlign = Render::UiTextVAlign::Middle;
    item.fontSize = fontSize;
    item.color = Render::Color::fromRGB255(
        color.red(), color.green(), color.blue(), color.alpha());
    item.zOrder = 15.0f;
    item.hasBackground = true;
    item.bgColor = Render::Color::fromRGB255(255, 255, 255, 230);
    item.bgPaddingX = 4.0f;
    item.bgPaddingY = 2.0f;
    item.bgRadius = 3.0f;
    m_uiTexts.push_back(item);
    update();
}
