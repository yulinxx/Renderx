#pragma once

/**
 * @file GizmoRenderer.h
 * @brief 3D 变换手柄渲染器
 *
 * 负责渲染平移、旋转、缩放手柄的几何体
 * 纯OpenGL渲染，不包含算法逻辑
 */

#include "RenderAPI.h"
#include "GLVerDef.h"
#include "Engine3D/Selection/SelectionManager3D.h"
#include "Render3D/Camera3D.h"

#include <QOpenGLShaderProgram>
#include <QOpenGLFunctions>

/**
 * @brief 3D Gizmo 渲染器
 *
 * 绘制变换手柄的可视化元素：
 * - 平移：三个彩色箭头 (RGB = XYZ)
 * - 旋转：三个彩色圆环 (RGB = XYZ)
 * - 缩放：三个彩色立方体手柄 (RGB = XYZ)
 */
class RENDER_API GizmoRenderer : protected XGLFunctions
{
public:
    GizmoRenderer();
    ~GizmoRenderer();

    GizmoRenderer(const GizmoRenderer&) = delete;
    GizmoRenderer& operator=(const GizmoRenderer&) = delete;

    /**
     * @brief 初始化OpenGL资源
     */
    void initialize();

    /**
     * @brief 渲染Gizmo
     * @param mode 当前变换模式
     * @param position 手柄位置（世界坐标，选中实体中心）
     * @param camera 相机
     * @param aspectRatio 视口宽高比
     * @param gizmoSize 手柄大小（世界空间）
     */
    void render(Eg::TransformMode mode,
        const Ut::Vec3f& position,
        const Camera3D& camera,
        float aspectRatio,
        float gizmoSize = 1.0f);

    /**
     * @brief 设置选中轴的高亮状态
     */
    void setHighlightAxis(Eg::TransformAxis axis);

    /**
     * @brief 释放OpenGL资源
     */
    void cleanup();

private:
    void createArrowGeometry();
    void createRingGeometry();
    void createCubeGeometry();
    void initShader();

    void renderArrow(const Ut::Vec3f& position,
        const Ut::Vec3f& direction,
        const Ut::Vec3f& color,
        const Ut::Mat4f& viewProj,
        float size,
        bool highlighted);

    void renderRing(const Ut::Vec3f& position,
        const Ut::Vec3f& normal,
        const Ut::Vec3f& color,
        const Ut::Mat4f& viewProj,
        float size,
        bool highlighted);

    void renderCube(const Ut::Vec3f& position,
        const Ut::Vec3f& color,
        const Ut::Mat4f& viewProj,
        float size,
        bool highlighted);

private:
    QOpenGLShaderProgram* m_shader = nullptr;

    // 箭头几何体
    GLuint m_arrowVao = 0;
    GLuint m_arrowVbo = 0;
    int m_arrowVertexCount = 0;

    // 圆环几何体
    GLuint m_ringVao = 0;
    GLuint m_ringVbo = 0;
    int m_ringVertexCount = 0;

    // 立方体几何体
    GLuint m_cubeVao = 0;
    GLuint m_cubeVbo = 0;
    int m_cubeVertexCount = 0;

    // 高亮状态
    Eg::TransformAxis m_highlightAxis = Eg::TransformAxis::None;
};