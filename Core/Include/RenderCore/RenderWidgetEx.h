#pragma once

/**
 * @file RenderWidgetEx.h
 * @brief 基于新渲染架构的Qt OpenGL Widget
 *
 * 特性：
 * 1. 支持2D/3D视图
 * 2. 与RenderWorld共享数据
 * 3. 增量更新机制
 * 4. 多窗口支持
 */

#include "RenderCore/RenderView.h"
#include "RenderCore/RenderWorld.h"

#include <QOpenGLWidget>
#include <QOpenGLFunctions_4_6_Core>
#include <QTimer>
#include <memory>

class QOpenGLShaderProgram;

namespace RenderCore
{

// ==================== 渲染Widget ========== =========

class RenderWidgetEx : public QOpenGLWidget, public IRenderView, protected QOpenGLFunctions_4_6_Core
{
    Q_OBJECT

public:
    explicit RenderWidgetEx(QWidget* parent = nullptr,
                           ViewManager* viewManager = nullptr,
                           EViewType viewType = EViewType::View2D);
    ~RenderWidgetEx() override;

    // ============ IRenderView 实现 ============

    QWidget* getWidget() const override { return this; }
    EViewType getViewType() const override { return m_viewType; }

    const Ut::Mat3f& getViewMatrix() const override { return m_viewMatrix; }
    void setViewMatrix(const Ut::Mat3f& matrix) override { m_viewMatrix = matrix; }

    RenderState getRenderState() const override;
    void requestUpdate() override;
    void onResize(int width, int height) override;

    // ============ 视图控制 ============

    /// 重置视图
    void resetView();

    /// 缩放
    void zoom(float factor);

    /// 平移
    void pan(float dx, float dy);

    /// 设置视图类型
    void setViewType(EViewType type) { m_viewType = type; }

    // ============ 实体操作接口 ==========

    /// 设置单个实体（增量更新）
    void setEntity(EntityId id,
                   std::vector<Vertex> vertices,
                   EPrimitiveType primitiveType = EPrimitiveType::Lines,
                   float lineWidth = 1.0f);

    /// 批量设置实体
    void setEntities(std::span<const EntityId> ids,
                     std::span<const std::vector<Vertex>> vertices,
                     std::span<const EPrimitiveType> primitiveTypes,
                     std::span<const float> lineWidths);

    /// 删除实体
    void removeEntity(EntityId id);

    /// 批量删除
    void removeEntities(std::span<const EntityId> ids);

    /// 清空所有
    void clearEntities();

signals:
    void entityCountChanged(size_t count);

protected:
    // QOpenGLWidget override
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

private:
    void initShaders();
    void initGeometryBuffers();

private:
    // 视图管理器
    ViewManager* m_viewManager = nullptr;

    // 视图类型
    EViewType m_viewType = EViewType::View2D;

    // 视图矩阵
    Ut::Mat3f m_viewMatrix;

    // 视口尺寸
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;

    // 缩放和平移
    float m_scale = 1.0f;
    QPointF m_translation;

    // 着色器程序
    GLuint m_sceneProgram = 0;
    GLint m_locViewMatrix = -1;

    // VAO/VBO
    GLuint m_vao = 0;

    // 脏标记
    bool m_dirtyWorld = true;

    // 帧率控制
    QTimer m_updateTimer;
    int m_targetFPS = 60;
};

} // namespace RenderCore
