#pragma once

#include "RenderAPI.h"
#include "GLVerDef.h"
#include "Render3D/Camera3D.h"
#include "Engine3D/Selection/SelectionManager3D.h"

#include <QOpenGLWidget>
#include <QOpenGLShaderProgram>
#include <QMouseEvent>
#include <QWheelEvent>
#include <vector>
#include <memory>

namespace Eg
{
    class SceneManager3D;
    struct SyMeshEntity;
}

class GizmoRenderer;

/**
 * @brief 3D OpenGL 渲染部件
 *
 * 支持轨道相机交互（旋转/平移/缩放）和 Phong 光照渲染
 * 支持模型选择和变换操作（移动/旋转/缩放）及Gizmo可视化
 */
class RENDER_API RenderWidget3D : public QOpenGLWidget, protected XGLFunctions
{
    Q_OBJECT

public:
    explicit RenderWidget3D(QWidget* parent = nullptr);
    ~RenderWidget3D() override;

    // ==================== 场景设置 ====================

    void setSceneManager(Eg::SceneManager3D* sceneManager);

    // ==================== 相机交互 ====================

    Camera3D& camera();
    const Camera3D& camera() const;
    void resetView();
    void fitAll();
    void setViewPreset(Camera3D::ViewPreset preset);

    // ==================== 渲染选项 ====================

    void setWireframeMode(bool enabled);
    bool isWireframeMode() const;
    void setShowGrid(bool visible);
    void setShowBBox(bool visible);
    void setShowFloor(bool visible);
    bool isShowFloor() const;

    // ==================== 选择管理 ====================

    Eg::SelectionManager3D& selectionManager();
    const Eg::SelectionManager3D& selectionManager() const;

    // ==================== 变换模式 ====================

    void setTransformMode(Eg::TransformMode mode);
    Eg::TransformMode getTransformMode() const;

signals:
    void sigSceneChanged();
    void sigCameraChanged();
    void sigSelectionChanged(const std::vector<Eg::SyMeshEntity*>&);

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;

    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    void initShaders();
    void initGridGeometry();
    void initBBoxGeometry();
    void initSelectionGeometry();
    void initFloorGeometry();

    void renderMeshEntities();
    void renderGrid();
    void renderBBox(const Ut::Vec3f& bboxMin, const Ut::Vec3f& bboxMax);
    void renderSelectionHighlight();
    void renderFloor();
    void renderGizmo();
    void updateMeshBuffers(const Eg::SyMeshEntity* entity,
        GLuint& vao, GLuint& vbo, GLuint& ebo,
        size_t& vertexCount, size_t& indexCount);

    /**
     * @brief 实体 GPU 缓存条目（避免每帧重建）
     */
    struct MeshGPUCache
    {
        GLuint vao = 0;
        GLuint vbo = 0;
        GLuint ebo = 0;
        size_t vertexCount = 0;
        size_t indexCount = 0;
        size_t vboCapacity = 0;        // 当前 VBO 容量（字节）
        uint64_t entityGeneration = 0; // 实体版本号，用于检测数据变化
    };

    /// 使用 per-entity 缓存和脏标记更新 GPU 缓冲区（仅数据变化时上传）
    void updateMeshBuffersCache(const Eg::SyMeshEntity* entity, MeshGPUCache& cache);

    /**
     * @brief 将屏幕坐标转换为世界空间射线
     */
    Eg::Ray3f screenToWorldRay(int x, int y) const;

    /**
     * @brief 判断实体是否被选中
     */
    bool isEntitySelected(const Eg::SyMeshEntity* entity) const;

private:
    // 相机
    Camera3D m_camera;

    // 场景
    Eg::SceneManager3D* m_sceneManager = nullptr;
    Eg::SelectionManager3D m_selectionManager;

    // Gizmo渲染器
    std::unique_ptr<GizmoRenderer> m_gizmoRenderer;

    // 着色器
    QOpenGLShaderProgram* m_meshProgram = nullptr;
    QOpenGLShaderProgram* m_gridProgram = nullptr;
    QOpenGLShaderProgram* m_bboxProgram = nullptr;
    QOpenGLShaderProgram* m_highlightProgram = nullptr;

    // 网格几何体
    GLuint m_gridVao = 0;
    GLuint m_gridVbo = 0;
    int m_gridVertexCount = 0;

    // 地板几何体
    GLuint m_floorVao = 0;
    GLuint m_floorVbo = 0;
    int m_floorVertexCount = 0;

    // 包围盒几何体
    GLuint m_bboxVao = 0;
    GLuint m_bboxVbo = 0;

    // 渲染状态
    bool m_wireframeMode = false;
    bool m_showGrid = true;
    bool m_showBBox = true;
    bool m_showSelectionHighlight = true;
    bool m_showFloor = true;

    // 鼠标交互状态
    enum class InteractionMode
    {
        None, Rotate, Pan, Select, Transform
    };
    InteractionMode m_interactionMode = InteractionMode::None;
    QPoint m_lastMousePos;
    Qt::MouseButtons m_pressedButtons;
    bool m_ctrlPressed = false;

    // 视口
    float m_aspectRatio = 1.0f;

    // 实体 GPU 缓存（避免每帧重传静态网格数据）
    std::unordered_map<const Eg::SyMeshEntity*, MeshGPUCache> m_meshCache;
    bool m_meshCacheDirty = true;  // 场景变化时清除缓存
};