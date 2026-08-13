/**
 * @file render_runtime.cpp
 * @brief RenderRuntime 实现
 */
#include "render_runtime.h"

#include <mutex>

namespace render
{
    // 静态实例 + 互斥锁用于线程安全的单例访问
    static std::once_flag g_onceFlag;
    static std::unique_ptr<RenderRuntime> g_runtime;
    static std::mutex g_instanceMutex;

    RenderRuntime& RenderRuntime::instance()
    {
        std::call_once(g_onceFlag, []() {
            g_runtime = std::unique_ptr<RenderRuntime>(new RenderRuntime());
        });
        return *g_runtime;
    }

    bool RenderRuntime::initialize(const std::string& dir, BackendType backend)
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_initialized)
        {
            return true;
        }

        shaderDir = dir;
        m_backend = backend;

        // 加载 shader 源码到全局缓存
        // NOTE: 当前仍调用 shader::initialize()，后续将完全迁移到 RenderRuntime 管理
        shader::initialize(dir);

        m_initialized = true;
        return true;
    }

    void RenderRuntime::shutdown()
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (!m_initialized)
        {
            return;
        }

        m_initialized = false;
        shaderDir.clear();
    }
}  // namespace render