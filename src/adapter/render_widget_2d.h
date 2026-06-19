#pragma once

#include <QOpenGLWidget>
#include <QColor>
#include "render/render.h"

namespace render { struct RenderDevice; }

class RENDER_API RenderWidget2D : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit RenderWidget2D(QWidget* parent = nullptr);
    ~RenderWidget2D() override;

    render::RenderDevice* device() const { return m_device; }

    void setViewMatrix(const float matrix[9]);
    void updateSceneEnvGeometry();

    void setDrawingColor(const QColor&) {}
    void setAntialiasing() {}
    void setWireframeMode() {}
    void setDepthTest() {}
    void resetView() {}
    void releaseGLResources() {}
    void setPreviewPoints(const void*, int) {}
    void setControlLines(const void*, int, uint32_t = 0) {}
    void update() { QWidget::update(); }

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    render::RenderDevice* m_device = nullptr;
    bool m_glInitialized = false;
};
