#include "RenderCore/RenderView.h"

#include <algorithm>

namespace RenderCore
{

// ==================== ViewManager 实现 ========== =========

ViewManager::ViewManager()
{
}

ViewManager::~ViewManager()
{
    shutdown();
}

bool ViewManager::initialize()
{
    if (m_initialized)
        return true;

    // 创建共享的RenderWorld
    m_world = std::make_unique<RenderWorld>();
    if (!m_world->initialize(EBackendType::OpenGL46))
        return false;

    m_initialized = true;
    return true;
}

void ViewManager::shutdown()
{
    if (!m_initialized)
        return;

    m_views.clear();
    m_world.reset();
    m_initialized = false;
}

void ViewManager::registerView(IRenderView* view)
{
    if (!view)
        return;

    // 确保不重复注册
    auto it = std::find(m_views.begin(), m_views.end(), view);
    if (it != m_views.end())
        return;

    m_views.push_back(view);
}

void ViewManager::unregisterView(IRenderView* view)
{
    if (!view)
        return;

    auto it = std::find(m_views.begin(), m_views.end(), view);
    if (it != m_views.end())
    {
        m_views.erase(it);
    }
}

void ViewManager::renderView(IRenderView* view)
{
    if (!view || !m_world || !m_initialized)
        return;

    makeCurrent(view);

    // 前置回调
    if (m_preRenderCb)
    {
        m_preRenderCb(view);
    }

    // 渲染World到当前视图
    RenderState state = view->getRenderState();
    m_world->render(state);

    // 后置回调
    if (m_postRenderCb)
    {
        m_postRenderCb(view);
    }
}

void ViewManager::renderAllViews()
{
    if (!m_world)
        return;

    for (IRenderView* view : m_views)
    {
        renderView(view);
    }
}

void ViewManager::notifyWorldUpdated()
{
    // World数据已更新，标记所有视图需要重绘
    for (IRenderView* view : m_views)
    {
        view->requestUpdate();
    }
}

void ViewManager::update()
{
    // 更新World（产生增量更新）
    if (m_world)
    {
        m_world->update();
    }

    // 渲染所有视图
    renderAllViews();
}

void ViewManager::makeCurrent(IRenderView* view)
{
    QWidget* widget = view->getWidget();
    if (widget)
    {
        widget->makeCurrent();
    }
}

} // namespace RenderCore
