#pragma once

#include "RenderAPI.h"
#include "RenderDataConsumer.h"
#include "Render/RenderTypes.h"
#include "GLVerDef.h"

#include <QOpenGLWidget>
#include <QString>
#include <vector>
#include <memory>
#include <type_traits>
#include <utility>

class QOpenGLShaderProgram;

/**
 * @brief OpenGL 渲染部件。
 *
 * 所有渲染数据通过 Render 层稳定数据契约传入，Render 只负责绘制。
 *
 * 渲染顺序：
 *   1. 场景环境几何（台面/网格/标尺）
 *   2. 位图
 *   3. 图元数据（RenderCommandList）
 *   4. 预览线 / 控制线 / 点标记 / 选择框 / 十字光标 / 捕捉标记等 UI 装饰
 *   5. UI 文字（标尺刻度数字、坐标提示、测量标注、屏幕固定文字等，QPainter 绘制）
 */
class RENDER_API RenderWidget : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit RenderWidget(QWidget* parent = nullptr);
    ~RenderWidget();

    /// 在窗口仍有效时主动释放 GL 资源（closeEvent / 父控件析构时调用，可重复调用）
    void releaseGLResources();

    // 视图控制
    void setViewMatrix(const Ut::Mat3f& matrix);
    const Ut::Mat3f& viewMatrix() const
    {
        return m_viewMatrix;
    }
    void setDrawingColor(const QColor& color);
    void setAntialiasing();
    void setWireframeMode();
    void setDepthTest();
    void resetView();

    // 场景渲染数据
    void setSceneCommands(Render::RenderCommandList&& cmdList);

    // 预览/控制线
    void setPreviewPoints(const Render::Vec2f* points, size_t count);
    void setControlLines(const Render::Vec2f* points, size_t count);
    void setControlLines(const Render::Vec2f* points, size_t count, const QColor& color);

    template <typename Vec2Like>
    void setPreviewPoints(const Vec2Like* points, size_t count)
    {
        const auto converted = convertPointArray(points, count);
        setPreviewPoints(converted.data(), converted.size());
    }

    template <typename Vec2Like>
    void setControlLines(const Vec2Like* points, size_t count)
    {
        const auto converted = convertPointArray(points, count);
        setControlLines(converted.data(), converted.size());
    }

    template <typename Vec2Like>
    void setControlLines(const Vec2Like* points, size_t count, const QColor& color)
    {
        const auto converted = convertPointArray(points, count);
        setControlLines(converted.data(), converted.size(), color);
    }

    // 屏幕空间固定大小的点标记（用于选择手柄）
    void setPointMarkers(const Render::Vec2f* worldPoints, size_t count,
        float markerSize = 6.0f,
        const QColor& fillColor = QColor(255, 255, 255),
        const QColor& borderColor = QColor(0, 0, 200));

    template <typename Vec2Like>
    void setPointMarkers(const Vec2Like* worldPoints, size_t count,
        float markerSize = 6.0f,
        const QColor& fillColor = QColor(255, 255, 255),
        const QColor& borderColor = QColor(0, 0, 200))
    {
        const auto converted = convertPointArray(worldPoints, count);
        setPointMarkers(converted.data(), converted.size(), markerSize, fillColor, borderColor);
    }

    // 选择框
    void setSelectionBox(const Render::BBox2d* bbox, const QColor& color);

    template <typename BBoxLike>
    void setSelectionBox(const BBoxLike* bbox, const QColor& color)
    {
        if (!bbox || !bbox->isValid())
        {
            setSelectionBox(static_cast<const Render::BBox2d*>(nullptr), color);
            return;
        }

        const Render::BBox2d converted(
            static_cast<double>(bbox->minPt.x()),
            static_cast<double>(bbox->minPt.y()),
            static_cast<double>(bbox->maxPt.x()),
            static_cast<double>(bbox->maxPt.y()));
        setSelectionBox(&converted, color);
    }

    // 选择手柄
    void setSelectionHandles(const Render::Vec2f* worldPoints, size_t count,
        float markerSize = 6.0f,
        const QColor& fillColor = QColor(255, 255, 255),
        const QColor& borderColor = QColor(0, 0, 200));

    template <typename Vec2Like>
    void setSelectionHandles(const Vec2Like* worldPoints, size_t count,
        float markerSize = 6.0f,
        const QColor& fillColor = QColor(255, 255, 255),
        const QColor& borderColor = QColor(0, 0, 200))
    {
        const auto converted = convertPointArray(worldPoints, count);
        setSelectionHandles(converted.data(), converted.size(), markerSize, fillColor, borderColor);
    }

    // 清除所有选择相关的绘制
    void clearSelectionDecoration();
    void clearSelectionPreview();

    // 鼠标事件接口
    void onMousePressEvent(QMouseEvent* event);
    void onMouseMoveEvent(QMouseEvent* event);
    void onMouseReleaseEvent(QMouseEvent* event);
    void onWheelEvent(QWheelEvent* event);

    // 鼠标位置更新
    void setMouseWorldPos(const QPointF& worldPos);

    // 位图贴图（使用四个角点定义显示区域，支持旋转、斜切等变换）
    bool setBitmapRGBA(const unsigned char* rgba, int w, int h,
        float tlX, float tlY, float trX, float trY,
        float blX, float blY, float brX, float brY);
    // 仅更新位图显示位置（不重新上传纹理，用于 gizmo 变换时的高频调用）
    void setBitmapPosition(float tlX, float tlY, float trX, float trY,
        float blX, float blY, float brX, float brY);
    void clearBitmap();

    // 捕捉指示
    void setSnapIndicator(const QPointF& worldPos, bool visible);
    void clearSnapIndicator();
    void setSnapIndicatorColor(const QColor& color);

    // —— 场景环境几何：纯数据驱动 ——
    void setSceneEnvGeometry(const Render::SceneEnvGeometry& geo);

    template <typename SceneEnvGeometryLike>
    void setSceneEnvGeometry(const SceneEnvGeometryLike& geo)
    {
        Render::SceneEnvGeometry converted;
        converted.layers.reserve(geo.layers.size());
        for (const auto& layer : geo.layers)
        {
            Render::SceneEnvLayer outLayer;
            outLayer.vertices.reserve(layer.vertices.size());
            for (const auto& v : layer.vertices)
                outLayer.vertices.emplace_back(static_cast<float>(v.x()), static_cast<float>(v.y()));

            outLayer.color = Render::Color(layer.color.r(), layer.color.g(), layer.color.b(), layer.color.a());
            outLayer.zDepth = layer.zDepth;
            outLayer.lineWidth = layer.lineWidth;
            outLayer.asTriangles = layer.asTriangles;
            outLayer.usePixelCoords = layer.usePixelCoords;
            converted.layers.push_back(std::move(outLayer));
        }

        converted.texts.reserve(geo.texts.size());
        for (const auto& text : geo.texts)
        {
            Render::UiTextItem outText;
            outText.text = text.text;
            outText.x = text.x;
            outText.y = text.y;
            outText.coordMode = static_cast<Render::UiTextCoordMode>(
                static_cast<std::underlying_type_t<decltype(text.coordMode)>>(text.coordMode));
            outText.hAlign = static_cast<Render::UiTextHAlign>(
                static_cast<std::underlying_type_t<decltype(text.hAlign)>>(text.hAlign));
            outText.vAlign = static_cast<Render::UiTextVAlign>(
                static_cast<std::underlying_type_t<decltype(text.vAlign)>>(text.vAlign));
            outText.fontSize = text.fontSize;
            outText.color = Render::Color(text.color.r(), text.color.g(), text.color.b(), text.color.a());
            outText.rotationDeg = text.rotationDeg;
            outText.zOrder = text.zOrder;
            outText.hasBackground = text.hasBackground;
            outText.bgColor = Render::Color(text.bgColor.r(), text.bgColor.g(), text.bgColor.b(), text.bgColor.a());
            outText.bgPaddingX = text.bgPaddingX;
            outText.bgPaddingY = text.bgPaddingY;
            outText.bgRadius = text.bgRadius;
            outText.hasLeaderLine = text.hasLeaderLine;
            outText.leaderLineColor = Render::Color(text.leaderLineColor.r(), text.leaderLineColor.g(),
                text.leaderLineColor.b(), text.leaderLineColor.a());
            outText.leaderLineWidth = text.leaderLineWidth;
            converted.texts.push_back(std::move(outText));
        }

        setSceneEnvGeometry(converted);
    }

    // ===== UI 文字：屏幕固定 + 世界定位（大小均为像素大小，不随缩放变化）=====

    /// 设置/替换通用 UI 文字列表（屏幕固定或世界定位，由 coordMode 决定）
    void setUiTexts(const Render::UiTextItemList& texts);
    void setUiTexts(Render::UiTextItemList&& texts);

    /// 添加单个 UI 文字项
    void addUiText(const Render::UiTextItem& text);

    /// 清除所有通用 UI 文字
    void clearUiTexts();

    /// 便捷方法：添加屏幕固定文字（像素坐标，左上角为原点，Y 向下）
    void addScreenText(const std::string& text,
        float pixelX, float pixelY,
        int fontSize = 12,
        const QColor& color = QColor(30, 30, 30),
        Render::UiTextHAlign hAlign = Render::UiTextHAlign::Left,
        Render::UiTextVAlign vAlign = Render::UiTextVAlign::Top);

    /// 便捷方法：添加世界坐标定位文字（位置跟随视图，但字号保持像素大小）
    void addWorldAnchorText(const std::string& text,
        float worldX, float worldY,
        int fontSize = 12,
        const QColor& color = QColor(30, 30, 30),
        Render::UiTextHAlign hAlign = Render::UiTextHAlign::Center,
        Render::UiTextVAlign vAlign = Render::UiTextVAlign::Middle,
        float rotationDeg = 0.0f);

    /// 便捷方法：显示当前鼠标世界坐标（在视口右下角）
    void setMouseCoordinateDisplay(bool on, const QColor& color = QColor(60, 60, 60));

    /// 便捷方法：设置/替换测量标注文字（世界锚点文字 + 可选指示线）
    void setMeasurementText(const std::string& text,
        float worldX, float worldY,
        int fontSize = 11,
        const QColor& color = QColor(200, 50, 50));

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    template <typename Vec2Like>
    static std::vector<Render::Vec2f> convertPointArray(const Vec2Like* points, size_t count)
    {
        std::vector<Render::Vec2f> converted;
        if (!points || count == 0)
            return converted;

        converted.reserve(count);
        for (size_t i = 0; i < count; ++i)
            converted.emplace_back(static_cast<float>(points[i][0]), static_cast<float>(points[i][1]));
        return converted;
    }

    void init();
    void updateLineBuffer();
    void updateCrossBuffer();
    void updateSnapGeometry();
    void initBitmapPipeline();
    void updateBitmapQuad();
    void renderSceneEnvGeo();
    void paintUiTexts();   // 用 QPainter 绘制所有 UI 文字（标尺 + 通用）

private:
    // --- 着色器 / VAO/VBO ---
    // ShaderManager 统一管理着色器生命周期，这里只缓存非拥有指针
    QOpenGLShaderProgram* m_flatProgram = nullptr;    // 场景环境几何共用着色器
    GLuint m_flatVao = 0;
    GLuint m_flatVbo = 0;

    QOpenGLShaderProgram* m_lineProgram = nullptr;
    GLuint m_lineVao = 0;
    GLuint m_lineVbo = 0;

    // 视图参数
    float m_scale = 1.0f;
    QVector2D m_translation;
    QPoint m_lastPos;

    Ut::Mat3f m_viewMatrix;
    QColor m_drawingColor;
    bool m_bAntialiasing = false;
    bool m_bWireframeMode = false;
    bool m_bDepthTest = false;

    // 预览线点
    std::vector<Render::Vec2f> m_linePoints;

    // 控制点辅助线
    std::vector<Render::Vec2f> m_ctrlLines;
    QColor m_ctrlLineColor{ 0, 0, 200, 200 };
    GLuint m_controlVao = 0;
    GLuint m_controlVbo = 0;
    QOpenGLShaderProgram* m_controlProgram = nullptr;

    struct PointMarker
    {
        Render::Vec2f worldPos;
    };
    std::vector<PointMarker> m_pointMarkers;
    float m_pointMarkerSize = 6.0f;
    QColor m_pointMarkerFill{ 255, 255, 255 };
    QColor m_pointMarkerBorder{ 0, 0, 200 };
    GLuint m_pointMarkerVao = 0;
    GLuint m_pointMarkerVbo = 0;

    bool m_hasSelectionBox = false;
    Render::BBox2d m_selectionBox;
    QColor m_selectionBoxColor{ 255, 140, 0, 220 };
    GLuint m_selectionBoxVao = 0;   // 持久化选择框 VAO
    GLuint m_selectionBoxVbo = 0;   // 持久化选择框 VBO

    std::vector<PointMarker> m_selectionHandles;
    float m_selectionHandleSize = 6.0f;
    QColor m_selectionHandleFill{ 255, 255, 255 };
    QColor m_selectionHandleBorder{ 0, 0, 200 };
    GLuint m_selectionHandleVao = 0;   // 持久化选择手柄 VAO
    GLuint m_selectionHandleVbo = 0;   // 持久化选择手柄 VBO

    std::vector<Render::Vec2f> m_crossPoints;
    GLuint m_crossVao = 0;
    GLuint m_crossVbo = 0;
    QOpenGLShaderProgram* m_crossProgram = nullptr;

    // 位图贴图显示
    QOpenGLShaderProgram* m_bitmapProgram = nullptr;
    GLuint m_bitmapVao = 0;
    GLuint m_bitmapVbo = 0;
    GLuint m_bitmapTexture = 0;
    bool m_bBitmap = false;
    int m_bitmapW = 0;
    int m_bitmapH = 0;
    float m_bitmapTlX = 0.0f, m_bitmapTlY = 0.0f;
    float m_bitmapTrX = 0.0f, m_bitmapTrY = 0.0f;
    float m_bitmapBlX = 0.0f, m_bitmapBlY = 0.0f;
    float m_bitmapBrX = 0.0f, m_bitmapBrY = 0.0f;

    // 鼠标位置跟踪
    QPoint m_mousePos;
    bool m_bMouseTracking = false;
    QPointF m_mouseWorldPos;

    // 捕捉标记
    std::vector<Render::Vec2f> m_snapPoints;
    GLuint m_snapVao = 0;
    GLuint m_snapVbo = 0;
    QOpenGLShaderProgram* m_snapProgram = nullptr;
    QColor m_snapColor{ 0x00, 0xFF, 0x00 };
    bool m_bSnapIndicator = false;
    QPointF m_snapWorldPos;

    RenderDataConsumer m_sceneConsumer;
    Render::RenderCommandList m_sceneCommands;
    bool m_bSceneData = false;

    // —— 场景环境几何（Render 数据契约）——
    Render::SceneEnvGeometry m_sceneEnvGeo;

    // —— 通用 UI 文字（用户通过 API 设置，与场景环境文字分开存储，
    //    以便在 paintEvent 中统一排序绘制）——
    Render::UiTextItemList m_uiTexts;

    // 坐标提示显示
    bool m_bShowMouseCoord = false;
    QColor m_mouseCoordColor{ 60, 60, 60 };

    XGLFunctions* m_glFuncs = nullptr;   // 由 QOpenGLContext 管理，不随 widget 析构
    bool m_glInitialized = false;
    bool m_glResourcesReleased = false;
};
