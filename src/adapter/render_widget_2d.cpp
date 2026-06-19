#include "render_widget_2d.h"
#include "render/render.h"

#include <QOpenGLContext>

RenderWidget2D::RenderWidget2D(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(QOpenGLWidget::PartialUpdate);
}

RenderWidget2D::~RenderWidget2D()
{
    makeCurrent();
    if (m_device)
    {
        renderDestroyDevice(m_device);
        m_device = nullptr;
    }
    doneCurrent();
}

void RenderWidget2D::initializeGL()
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

void RenderWidget2D::resizeGL(int w, int h)
{
    if (!m_device) return;
    renderResize(m_device, static_cast<uint32_t>(w * devicePixelRatio()),
                 static_cast<uint32_t>(h * devicePixelRatio()));
}

void RenderWidget2D::paintGL()
{
    if (!m_device) return;
    renderFrame(m_device);
}

void RenderWidget2D::setViewMatrix(const float matrix[9])
{
    if (!m_device) return;
    float vpW = static_cast<float>(width());
    float vpH = static_cast<float>(height());
    renderSetView2D(m_device, matrix, vpW, vpH);
}
