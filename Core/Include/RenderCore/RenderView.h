#pragma once

/**
 * @file RenderView.h
 * @brief 多窗口渲染视图接口
 *
 * 设计原则：
 * 1. 多窗口共享同一个RenderWorld
 * 2. 每个视图有独立的视图矩阵和渲染状态
 * 3. 支持2D和3D视图
 * 4. 与Qt无缝集成
 */

#include "RenderCore/RenderWorld.h"
#include "Render/RenderTypes.h"

#include <memory>
#include <functional>
#include <QWidget>
#include <QOpenGLContext>

namespace RenderCore
{

// ==================== 视图类型 ========== =========

enum class EViewType
{
    View2D,    // 2D正交视图
    View3D,    // 3D透视视图
};

// ==================== 视图接口 ========== =========

class IRenderView
{
public:
    virtual ~IRenderView() = default;

    /// 获取关联的Qt窗口
    virtual QWidget* getWidget() const = 0;

    /// 获取视图类型
    virtual EViewType getViewType() const = 0;

    /// 获取视图矩阵
    virtual const Ut::Mat3f& getViewMatrix() const = 0;

    /// 设置视图矩阵
    virtual void setViewMatrix(const Ut::Mat3f& matrix) = 0;

    /// 获取当前渲染状态
    virtual RenderState getRenderState() const = 0;

    /// 请求重绘
    virtual void requestUpdate() = 0;

    /// 窗口尺寸改变
    virtual void onResize(int width, int height) = 0;
};

// ==================== 视图管理器 ========== =========

class ViewManager
{
public:
    ViewManager();
    ~ViewManager();

    // ============ 禁止拷贝 ============

    ViewManager(const ViewManager&) = delete;
    ViewManager& operator=(const ViewManager&) = delete;

    // ============ 生命周期 ============

    /// 初始化（需要Qt OpenGL上下文）
    bool initialize();

    /// 销毁
    void shutdown();

    // ============ World管理 ============

    /// 获取共享的World
    RenderWorld* getWorld() { return m_world.get(); }
    const RenderWorld* getWorld() const { return m_world.get(); }

    // ============ 视图注册 ========== =========

    /// 注册视图
    void registerView(IRenderView* view);

    /// 注销视图
    void unregisterView(IRenderView* view);

    // ============ 渲染 ========== =========

    /// 渲染指定视图
    void renderView(IRenderView* view);

    /// 渲染所有视图
    void renderAllViews();

    // ============ 更新 ========== =========

    /// 通知World数据已更新
    void notifyWorldUpdated();

    /// 每帧调用
    void update();

    // ============ 回调 ========== =========

    using RenderCallback = std::function<void(IRenderView*)>;
    void setPreRenderCallback(RenderCallback cb) { m_preRenderCb = std::move(cb); }
    void setPostRenderCallback(RenderCallback cb) { m_postRenderCb = std::move(cb); }

private:
    void makeCurrent(IRenderView* view);

private:
    std::unique_ptr<RenderWorld> m_world;

    std::vector<IRenderView*> m_views;

    RenderCallback m_preRenderCb;
    RenderCallback m_postRenderCb;

    bool m_initialized = false;
};

// ==================== 智能指针类型 ========== =========

using ViewManagerPtr = std::unique_ptr<ViewManager>;

} // namespace RenderCore
