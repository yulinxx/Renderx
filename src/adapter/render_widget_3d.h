#pragma once

#include <QOpenGLWidget>
#include <QMouseEvent>
#include <QWheelEvent>

#include "render/render.h"
#include "selection_manager_3d.h"

namespace render { struct RenderDevice; }

namespace Eg {
    enum class TransformMode { None, Translate, Rotate, Scale };
    class SelectionManager3D;
}

class RENDER_API RenderWidget3DNew : public QOpenGLWidget
{
    Q_OBJECT

public:
    explicit RenderWidget3DNew(QWidget* parent = nullptr);
    ~RenderWidget3DNew() override;

    render::RenderDevice* device() const { return m_device; }

    void setViewMatrices(const float viewMatrix[16], const float projMatrix[16]);

    Eg::SelectionManager3D& selectionManager();
    void setTransformMode(Eg::TransformMode mode);

public slots:
    void resetView();
    void fitAll();
    void setWireframeMode(bool enabled);
    void setShowGrid(bool enabled);
    void setShowBBox(bool enabled);
    void setShowFloor(bool enabled);
    void setViewPreset(int preset);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    render::RenderDevice* m_device = nullptr;
    Eg::SelectionManager3D m_selectionManager;
    bool m_glInitialized = false;
    float m_lastMouseX = 0.0f;
    float m_lastMouseY = 0.0f;
};
