#include "render_widget_3d.h"
#include "render/render.h"

#include <QOpenGLContext>

RenderWidget3DNew::RenderWidget3DNew(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);
    setMouseTracking(true);
}

RenderWidget3DNew::~RenderWidget3DNew()
{
    makeCurrent();
    if (m_device)
    {
        renderDestroyDevice(m_device);
        m_device = nullptr;
    }
    doneCurrent();
}

void RenderWidget3DNew::initializeGL()
{
    if (m_glInitialized) return;

    auto* ctx = QOpenGLContext::currentContext();
    if (!ctx) return;

    render::DeviceDesc desc{};
    desc.backend = render::BackendType::OpenGL;
    desc.debugLayer = false;
    desc.nativeWindowHandle = nullptr;
    desc.width = static_cast<uint32_t>(width() * devicePixelRatio());
    desc.height = static_cast<uint32_t>(height() * devicePixelRatio());

    m_device = renderCreateDevice(&desc);
    m_glInitialized = (m_device != nullptr);
}

void RenderWidget3DNew::resizeGL(int w, int h)
{
    if (!m_device) return;
    renderResize(m_device, static_cast<uint32_t>(w * devicePixelRatio()),
                 static_cast<uint32_t>(h * devicePixelRatio()));
}

void RenderWidget3DNew::paintGL()
{
    if (!m_device) return;
    renderFrame(m_device);
}

void RenderWidget3DNew::setViewMatrices(const float viewMatrix[16], const float projMatrix[16])
{
    if (!m_device) return;
    renderSetView3D(m_device, viewMatrix, projMatrix);
}

void RenderWidget3DNew::mousePressEvent(QMouseEvent* event)
{
    m_lastMouseX = static_cast<float>(event->pos().x());
    m_lastMouseY = static_cast<float>(event->pos().y());
}

void RenderWidget3DNew::mouseMoveEvent(QMouseEvent* event)
{
    m_lastMouseX = static_cast<float>(event->pos().x());
    m_lastMouseY = static_cast<float>(event->pos().y());
}

void RenderWidget3DNew::mouseReleaseEvent(QMouseEvent* event)
{
}

void RenderWidget3DNew::wheelEvent(QWheelEvent* event)
{
}

void RenderWidget3DNew::resetView() {}
void RenderWidget3DNew::fitAll() {}
void RenderWidget3DNew::setWireframeMode(bool) {}
void RenderWidget3DNew::setShowGrid(bool) {}
void RenderWidget3DNew::setShowBBox(bool) {}
void RenderWidget3DNew::setShowFloor(bool) {}
void RenderWidget3DNew::setViewPreset(int) {}

Eg::SelectionManager3D& RenderWidget3DNew::selectionManager() { return m_selectionManager; }
void RenderWidget3DNew::setTransformMode(Eg::TransformMode) {}
